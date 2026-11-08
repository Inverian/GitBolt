#include "update/Checker.h"

#include "update/Request.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTimer>

#include <utility>

namespace gitbolt::update {

namespace {

// A releases/latest answer is a few kilobytes, notes included; GitHub
// caps a release body at 125,000 characters.
constexpr qint64 kMaxFeedBytes = 4 * 1024 * 1024;

} // namespace

Checker::Checker(Config config, QObject* parent) : QObject(parent), config_(std::move(config)) {}

Checker::~Checker() {
    // Disconnect BEFORE abort(): it emits finished() synchronously, and
    // nothing may be reported from a half-destroyed checker. The reply
    // itself goes with network_, its parent.
    if (QNetworkReply* reply = std::exchange(reply_, nullptr)) {
        reply->disconnect(this);
        reply->abort();
    }
}

const Config& Checker::config() const {
    return config_;
}

bool Checker::busy() const {
    return busy_;
}

void Checker::check(bool manual) {
    if (busy_) {
        manual_ = manual_ || manual;
        return;
    }
    busy_ = true;
    manual_ = manual;

    if (!config_.isUsable()) {
        // Queued like every other outcome, so no slot runs inside check().
        QTimer::singleShot(0, this, [this] {
            const bool wasManual = std::exchange(manual_, false);
            busy_ = false;
            emit failed(tr("Updates aren't set up in this copy of GitBolt."), wasManual);
        });
        return;
    }

    if (!network_) {
        network_ = new QNetworkAccessManager(this);
        network_->setRedirectPolicy(QNetworkRequest::ManualRedirectPolicy);
    }
    QNetworkRequest request = updateRequest(config_.feedUrl, config_);
    request.setRawHeader(QByteArrayLiteral("Accept"), QByteArrayLiteral("application/vnd.github+json"));
    request.setRawHeader(QByteArrayLiteral("X-GitHub-Api-Version"), QByteArrayLiteral("2022-11-28"));

    tooLarge_ = false;
    QNetworkReply* reply = network_->get(request);
    reply_ = reply;
    connect(reply, &QNetworkReply::downloadProgress, this, [this, reply](qint64 received, qint64 total) {
        if (reply != reply_ || tooLarge_)
            return;
        if (received > kMaxFeedBytes || total > kMaxFeedBytes) {
            tooLarge_ = true;
            reply->abort();   // finished(), and so onFinished, from inside this call
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply] { onFinished(reply); });
}

void Checker::onFinished(QNetworkReply* reply) {
    reply->deleteLater();
    if (reply != reply_)
        return;
    reply_ = nullptr;
    const bool manual = std::exchange(manual_, false);
    busy_ = false;
    report(reply, manual);
}

void Checker::report(QNetworkReply* reply, bool manual) {
    const auto notARelease = [](const QString& why) {
        return tr("GitHub's answer wasn't a GitBolt release: %1.").arg(why);
    };
    if (tooLarge_) {
        emit failed(notARelease(tr("it is too large")), manual);
        return;
    }

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    // GitHub's primary limit says x-ratelimit-remaining: 0; its
    // secondary limits say retry-after; either may come as 403 or 429.
    const bool limited =
        status == 429
        || (status == 403
            && (reply->rawHeader("x-ratelimit-remaining").trimmed() == "0" || reply->hasRawHeader("retry-after")));
    if (limited) {
        emit failed(tr("GitHub is limiting requests from your network; try again later."), manual);
        return;
    }
    if (status > 0 && status != 200) {
        emit failed(tr("GitHub answered the update check with HTTP %1.").arg(status), manual);
        return;
    }
    // No status: no answer at all. A 200 with an error: the answer was
    // cut off (the connection dropped, or the transfer timed out).
    if (status != 200 || reply->error() != QNetworkReply::NoError) {
        emit failed(tr("Couldn't reach GitHub: %1.").arg(reply->errorString()), manual);
        return;
    }

    const ParsedRelease parsed = parseRelease(reply->readAll(), config_.repo);
    if (!parsed.release) {
        emit failed(notARelease(parsed.error), manual);
        return;
    }
    if (parsed.release->version > config_.current)
        emit available(*parsed.release, manual);
    else
        emit upToDate(parsed.release->version, manual);
}

} // namespace gitbolt::update
