#include "telemetry/TelemetryService.h"

#include "telemetry/CrashLogParser.h"
#include "telemetry/GlitchTipSink.h"
#include "telemetry/OpenPanelSink.h"
#include "telemetry/Transport.h"

#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>

#include <algorithm>
#include <utility>

namespace gitbolt::telemetry {

namespace {

// Plenty for every action the window has; a cap all the same, so a
// bug that counts something unbounded can't grow the hash forever.
constexpr qsizetype kMaxDistinctCommands = 512;

} // namespace

TelemetryService::TelemetryService(Config config, std::function<QString()> installIdProvider,
                                   bool enabled, std::function<bool()> switchReader,
                                   QObject* parent)
    : QObject(parent),
      config_(std::move(config)),
      installIdProvider_(std::move(installIdProvider)),
      switchReader_(std::move(switchReader)),
      client_{config_.appVersion, Environment::current()},
      userAgent_(userAgent(client_)),
      complete_(config_.isComplete()),
      userEnabled_(enabled) {
    flushTimer_.setSingleShot(true);
    paceTimer_.setSingleShot(true);
    retryTimer_.setInterval(config_.flushIntervalMs);
    countTimer_.setInterval(config_.countWindowMs);
    connect(&flushTimer_, &QTimer::timeout, this, &TelemetryService::beginCycle);
    connect(&paceTimer_, &QTimer::timeout, this, &TelemetryService::sendFront);
    connect(&retryTimer_, &QTimer::timeout, this, &TelemetryService::onRetryTick);
    connect(&countTimer_, &QTimer::timeout, this, &TelemetryService::flushCounts);

    if (userEnabled_ && complete_)
        openQueue();
}

TelemetryService::~TelemetryService() {
    // persistPending() is the quit path; this only makes sure no reply
    // reaches a half-destroyed service.
    quitting_ = true;
    abortInFlight();
    stopTimers();
}

bool TelemetryService::isActive() const {
    return userEnabled_ && complete_ && !quitting_ && queue_ && queue_->isUsable();
}

qsizetype TelemetryService::queuedCount() const {
    return isActive() ? queue_->size() : 0;
}

void TelemetryService::openQueue() {
    queue_ = std::make_unique<EventQueue>(
        config_.queueDir, EventQueue::Limits{config_.maxQueued, config_.maxQueueBytes,
                                             config_.maxAgeDays, config_.maxAttempts});
    queue_->open();   // unusable when another live process holds it
}

void TelemetryService::start() {
    started_ = true;
    if (!isActive())
        return;
    retryTimer_.start();
    countTimer_.start();
    if (!queue_->isEmpty())
        scheduleFlush();
}

QString TelemetryService::installId() {
    if (!installIdFetched_) {
        installIdFetched_ = true;
        if (installIdProvider_)
            installId_ = installIdProvider_();
    }
    return installId_;
}

void TelemetryService::recordLaunch() {
    if (!isActive())
        return;
    if (std::optional<QJsonObject> event = appLaunchedEvent(client_, installId()))
        enqueue(Sink::OpenPanel, std::move(*event));
}

void TelemetryService::countCommand(const QString& objectName) {
    if (!isActive())
        return;
    const std::optional<QString> name = commandName(objectName);
    if (!name || (!counts_.contains(*name) && counts_.size() >= kMaxDistinctCommands))
        return;
    ++counts_[*name];
}

void TelemetryService::recordRepoOpened(qint64 openMs, qint64 historyMs, int firstPageCommits) {
    if (!isActive())
        return;
    if (std::optional<QJsonObject> event =
            repoOpenedEvent(client_, installId(), openMs, historyMs, firstPageCommits))
        enqueue(Sink::OpenPanel, std::move(*event));
}

void TelemetryService::recordError(ErrorKind kind, const QString& detail) {
    if (!isActive())
        return;
    const QString key = errorKindName(kind) + QLatin1Char('/') + errorDetail(kind, detail);
    if (reportedErrors_.contains(key) || reportedErrors_.size() >= config_.maxErrorsPerSession)
        return;
    reportedErrors_.insert(key);
    enqueue(Sink::GlitchTip,
            glitchTipError(client_, kind, detail, QDateTime::currentDateTimeUtc()));
}

bool TelemetryService::queueCrashReport(const QString& crashLog) {
    if (!isActive())
        return false;
    std::optional<CrashEvent> crash = CrashLogParser::parse(crashLog);
    if (!crash)
        return false;
    enqueue(Sink::GlitchTip, glitchTipCrash(*crash));
    return true;
}

void TelemetryService::enqueue(Sink sink, QJsonObject payload) {
    QueuedRecord record;
    record.sink = sink;
    record.queued = QDateTime::currentDateTimeUtc();
    record.payload = std::move(payload);
    queue_->append(std::move(record));
    if (started_)
        scheduleFlush();
}

void TelemetryService::scheduleFlush() {
    // Not during a cycle (it picks the record up), a backoff (the next
    // tick retries), or once this tick's budget is spent. Not
    // restarted by later records either: the first unsent one sets the
    // time, so a steady stream of records can't postpone sending.
    if (!isActive() || inCycle_ || backingOff_ || flushTimer_.isActive()
        || sentThisTick_ >= config_.maxPerFlush)
        return;
    // A crash summary (the queue keeps it in front) goes at once: the
    // crash may come back within seconds, and the next one replaces a
    // summary still queued, so a crash loop would never be reported.
    const bool crash = !queue_->isEmpty() && queue_->front().isCrash();
    flushTimer_.start(crash ? 0 : config_.firstFlushDelayMs);
}

void TelemetryService::beginCycle() {
    // backingOff_: a flush armed before a request failed would otherwise
    // send again before the next retry tick.
    if (!isActive() || inCycle_ || backingOff_)
        return;
    // Every cycle, the first flush's and each retry tick's, idle or
    // not: the switch may have been turned off in another process.
    if (switchedOffElsewhere())
        return;
    if (queue_->isEmpty()) {
        queue_->sync();
        return;
    }
    if (!network_) {
        network_ = new QNetworkAccessManager(this);
        network_->setRedirectPolicy(QNetworkRequest::ManualRedirectPolicy);
    }
    inCycle_ = true;
    sendFront();
}

void TelemetryService::sendFront() {
    if (!isActive() || !inCycle_)
        return;
    if (queue_->isEmpty() || sentThisTick_ >= config_.maxPerFlush) {
        endCycle();
        return;
    }

    const QueuedRecord& record = queue_->front();
    QJsonObject body = record.payload;
    QNetworkRequest request;
    if (record.sink == Sink::OpenPanel) {
        // Fresh events take the server's receipt time; only a record
        // that waited (a retry, an earlier session) says when it
        // happened. Both ends of the age are this client's clock.
        if (record.queued.msecsTo(QDateTime::currentDateTimeUtc()) > config_.timestampAfterMs)
            body = withTimestamp(std::move(body), record.queued);
        request = OpenPanelSink::request(config_, userAgent_);
    } else {
        request = GlitchTipSink::request(config_, userAgent_);
    }

    ++sentThisTick_;
    QNetworkReply* reply =
        network_->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    reply_ = reply;
    // By id, not "the front": a record appended meanwhile can push the
    // queue over its caps, which may drop this very record.
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, id = record.id] { onReplyFinished(reply, id); });
}

void TelemetryService::onReplyFinished(QNetworkReply* reply, quint64 recordId) {
    reply->deleteLater();
    // abort() emits finished() synchronously, inside the call. The
    // service's own aborts (off switch, quit, destruction) clear
    // reply_ and disconnect first; this is the belt to those braces.
    if (reply != reply_)
        return;
    reply_ = nullptr;
    if (!isActive())
        return;

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    switch (classify(status, reply->error())) {
    case Outcome::Sent:
    case Outcome::Drop:
        queue_->remove(recordId);
        paceTimer_.start(config_.paceMs);
        return;
    case Outcome::Retry:
        queue_->bumpAttempts(recordId);
        backingOff_ = true;
        endCycle();
        return;
    case Outcome::Unreachable:
        // As a Retry, but the record keeps its attempts: time offline
        // costs it nothing, and adds nothing for endCycle to write.
        backingOff_ = true;
        endCycle();
        return;
    }
}

void TelemetryService::endCycle() {
    inCycle_ = false;
    paceTimer_.stop();
    queue_->sync();   // the one rewrite of this cycle
}

void TelemetryService::onRetryTick() {
    sentThisTick_ = 0;
    backingOff_ = false;
    beginCycle();
}

void TelemetryService::flushCounts() {
    if (!isActive() || counts_.isEmpty())
        return;
    QStringList names = counts_.keys();
    names.sort();
    const QHash<QString, int> counts = std::exchange(counts_, {});
    for (const QString& name : std::as_const(names)) {
        if (std::optional<QJsonObject> event =
                commandUsedEvent(client_, installId(), name, counts.value(name)))
            enqueue(Sink::OpenPanel, std::move(*event));
    }
}

void TelemetryService::setUserEnabled(bool enabled) {
    if (enabled == userEnabled_)
        return;
    userEnabled_ = enabled;   // first: isActive() is false from here on

    if (!enabled) {
        abortInFlight();
        stopTimers();
        inCycle_ = false;
        backingOff_ = false;
        counts_.clear();
        if (queue_)
            queue_->close();   // releases the lock, writes nothing
        queue_.reset();
        EventQueue::discard(config_.queueDir);
        return;
    }

    if (!complete_ || quitting_)
        return;
    openQueue();
    if (started_ && isActive()) {
        retryTimer_.start();
        countTimer_.start();
        if (!queue_->isEmpty())
            scheduleFlush();
    }
}

void TelemetryService::persistPending() {
    // Turned off in another process since the last cycle: the queue is
    // deleted, as by the switch, rather than written once more.
    if (isActive())
        switchedOffElsewhere();
    if (!isActive()) {
        abortInFlight();
        stopTimers();
        return;
    }
    flushCounts();
    quitting_ = true;   // before the abort: nothing restarts from here
    abortInFlight();
    stopTimers();
    inCycle_ = false;
    queue_->sync();
}

void TelemetryService::abortInFlight() {
    // Clear and disconnect BEFORE abort(): it emits finished()
    // synchronously, and a handler that ran would count the abort as
    // a failed attempt (or pop the record) and restart the pacer.
    QNetworkReply* reply = std::exchange(reply_, nullptr);
    if (!reply)
        return;
    reply->disconnect(this);
    reply->abort();
    reply->deleteLater();
}

void TelemetryService::stopTimers() {
    flushTimer_.stop();
    retryTimer_.stop();
    paceTimer_.stop();
    countTimer_.stop();
}

bool TelemetryService::switchedOffElsewhere() {
    if (!switchReader_ || switchReader_())
        return false;
    setUserEnabled(false);   // stops, and deletes the queue
    return true;
}

} // namespace gitbolt::telemetry
