#include "telemetry/EventQueue.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLockFile>
#include <QSaveFile>
#include <QTimeZone>

#include <atomic>
#include <optional>

namespace gitbolt::telemetry {

namespace {

std::atomic<quint64> g_diskWrites{0};

QString sinkName(Sink sink) {
    return sink == Sink::OpenPanel ? QStringLiteral("openpanel") : QStringLiteral("glitchtip");
}

QByteArray serialize(const QueuedRecord& record) {
    QJsonObject line;
    line.insert(QStringLiteral("v"), 1);
    line.insert(QStringLiteral("sink"), sinkName(record.sink));
    line.insert(QStringLiteral("attempts"), record.attempts);
    line.insert(QStringLiteral("queued"),
                record.queued.toTimeZone(QTimeZone::UTC).toString(Qt::ISODateWithMs));
    line.insert(QStringLiteral("payload"), record.payload);
    return QJsonDocument(line).toJson(QJsonDocument::Compact) + '\n';
}

std::optional<QueuedRecord> deserialize(const QByteArray& line) {
    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(line, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        return std::nullopt;
    const QJsonObject object = doc.object();
    if (object.value(QStringLiteral("v")).toInt(-1) != 1)
        return std::nullopt;

    QueuedRecord record;
    const QString sink = object.value(QStringLiteral("sink")).toString();
    if (sink == QLatin1String("openpanel"))
        record.sink = Sink::OpenPanel;
    else if (sink == QLatin1String("glitchtip"))
        record.sink = Sink::GlitchTip;
    else
        return std::nullopt;
    record.attempts = object.value(QStringLiteral("attempts")).toInt(-1);
    record.queued = QDateTime::fromString(object.value(QStringLiteral("queued")).toString(),
                                          Qt::ISODateWithMs);
    const QJsonValue payload = object.value(QStringLiteral("payload"));
    if (record.attempts < 0 || !record.queued.isValid() || !payload.isObject())
        return std::nullopt;
    record.queued = record.queued.toTimeZone(QTimeZone::UTC);
    record.payload = payload.toObject();
    return record;
}

QString lockPath(const QString& dir) {
    return dir + QStringLiteral("/queue.lock");
}

} // namespace

bool QueuedRecord::isCrash() const {
    return sink == Sink::GlitchTip
           && payload.value(QStringLiteral("level")).toString() == QLatin1String("fatal");
}

EventQueue::EventQueue(QString dir, Limits limits) : dir_(std::move(dir)), limits_(limits) {}

EventQueue::~EventQueue() = default;

QString EventQueue::filePath(const QString& dir) {
    return dir + QStringLiteral("/queue.jsonl");
}

QString EventQueue::filePath() const {
    return filePath(dir_);
}

bool EventQueue::isUsable() const {
    return lock_ && lock_->isLocked();
}

bool EventQueue::open() {
    if (isUsable())
        return true;
    if (!QDir().mkpath(dir_))
        return false;
    lock_ = std::make_unique<QLockFile>(lockPath(dir_));
    lock_->setStaleLockTime(0);
    if (!lock_->tryLock(0)) {
        lock_.reset();
        return false;
    }

    records_.clear();
    lineBytes_.clear();
    totalBytes_ = 0;
    dirty_ = false;

    QFile file(filePath());
    if (!file.open(QIODevice::ReadOnly))
        return true;
    // A file far over the cap is not one this class wrote: start over.
    if (file.size() > 8 * limits_.maxBytes) {
        dirty_ = true;
        return true;
    }
    const QDateTime oldest =
        QDateTime::currentDateTimeUtc().addDays(-qint64(limits_.maxAgeDays));
    while (!file.atEnd()) {
        const QByteArray line = file.readLine().trimmed();
        if (line.isEmpty())
            continue;
        std::optional<QueuedRecord> record = deserialize(line);
        if (!record || record->queued < oldest || record->attempts >= limits_.maxAttempts) {
            dirty_ = true;
            continue;
        }
        if (record->isCrash()) {
            for (qsizetype i = 0; i < records_.size(); ++i) {
                if (records_.at(i).isCrash()) {
                    dropAt(i);
                    dirty_ = true;
                    break;
                }
            }
        }
        const qint64 bytes = serialize(*record).size();
        record->id = nextId_++;
        records_.append(std::move(*record));
        lineBytes_.append(bytes);
        totalBytes_ += bytes;
    }
    enforceCaps();
    return true;
}

void EventQueue::append(QueuedRecord record) {
    if (!isUsable())
        return;
    if (record.isCrash()) {
        for (qsizetype i = 0; i < records_.size(); ++i) {
            if (records_.at(i).isCrash()) {
                dropAt(i);
                dirty_ = true;
                break;
            }
        }
    }
    const QByteArray line = serialize(record);
    record.id = nextId_++;
    records_.append(std::move(record));
    lineBytes_.append(line.size());
    totalBytes_ += line.size();

    QFile file(filePath());
    if (file.open(QIODevice::WriteOnly | QIODevice::Append) && file.write(line) == line.size()
        && file.flush()) {
        ++g_diskWrites;
    } else {
        dirty_ = true;   // the next sync() writes it
    }
    enforceCaps();
}

void EventQueue::remove(quint64 id) {
    const qsizetype index = indexOf(id);
    if (index < 0)
        return;
    dropAt(index);
    dirty_ = true;
}

void EventQueue::bumpAttempts(quint64 id) {
    const qsizetype index = indexOf(id);
    if (index < 0)
        return;
    dirty_ = true;
    QueuedRecord& record = records_[index];
    if (++record.attempts >= limits_.maxAttempts) {
        dropAt(index);
        return;
    }
    const qint64 bytes = serialize(record).size();
    totalBytes_ += bytes - lineBytes_.at(index);
    lineBytes_[index] = bytes;
}

bool EventQueue::sync() {
    if (!isUsable() || !dirty_)
        return true;
    if (records_.isEmpty()) {
        if (QFile::exists(filePath())) {
            if (!QFile::remove(filePath()))
                return false;
            ++g_diskWrites;
        }
        dirty_ = false;
        return true;
    }
    QSaveFile file(filePath());
    if (!file.open(QIODevice::WriteOnly))
        return false;
    for (const QueuedRecord& record : std::as_const(records_))
        file.write(serialize(record));
    if (!file.commit())
        return false;
    ++g_diskWrites;
    dirty_ = false;
    return true;
}

void EventQueue::close() {
    lock_.reset();   // ~QLockFile unlocks, which removes queue.lock
    records_.clear();
    lineBytes_.clear();
    totalBytes_ = 0;
    dirty_ = false;
}

void EventQueue::discard(const QString& dir) {
    if (!QFileInfo::exists(filePath(dir)) && !QFileInfo::exists(lockPath(dir)))
        return;
    QLockFile lock(lockPath(dir));
    lock.setStaleLockTime(0);
    // Held by a live process: its session still uses the queue (that
    // process applies its own gate). tryLock clears a dead owner's lock.
    if (!lock.tryLock(0))
        return;
    QFile::remove(filePath(dir));
    lock.unlock();
    QDir().rmdir(dir);   // only if nothing else is in it
}

quint64 EventQueue::diskWrites() {
    return g_diskWrites.load();
}

void EventQueue::enforceCaps() {
    while (!records_.isEmpty()
           && (records_.size() > limits_.maxQueued || totalBytes_ > limits_.maxBytes)) {
        // Oldest analytics first, then the oldest error, the crash last.
        qsizetype victim = -1;
        for (qsizetype i = 0; i < records_.size() && victim < 0; ++i) {
            if (records_.at(i).sink == Sink::OpenPanel)
                victim = i;
        }
        for (qsizetype i = 0; i < records_.size() && victim < 0; ++i) {
            if (!records_.at(i).isCrash())
                victim = i;
        }
        dropAt(victim < 0 ? 0 : victim);
        dirty_ = true;
    }
}

qsizetype EventQueue::indexOf(quint64 id) const {
    // The record in flight is almost always the front.
    for (qsizetype i = 0; i < records_.size(); ++i) {
        if (records_.at(i).id == id)
            return i;
    }
    return -1;
}

void EventQueue::dropAt(qsizetype index) {
    totalBytes_ -= lineBytes_.at(index);
    lineBytes_.removeAt(index);
    records_.removeAt(index);
}

} // namespace gitbolt::telemetry
