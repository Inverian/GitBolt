#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>

#include <memory>

class QLockFile;

namespace gitbolt::telemetry {

/// Where a record goes.
enum class Sink { OpenPanel, GlitchTip };

/// One queued event: built completely when it was queued (a queued
/// event keeps the version that produced it), plus the bookkeeping the
/// sender needs.
struct QueuedRecord {
    Sink sink = Sink::OpenPanel;
    int attempts = 0;
    QDateTime queued;        ///< UTC
    QJsonObject payload;
    /// Set by the queue, in memory only: which record a reply belongs
    /// to. The caps may drop the record in flight before its reply
    /// comes, so "the front" can be another record by then.
    quint64 id = 0;

    /// A GlitchTip crash event: the queue keeps at most one, and drops
    /// it last when over its caps.
    bool isCrash() const;
};

/// The bounded on-disk queue, <queueDir>/queue.jsonl, one JSON object
/// per line:
///   {"v":1,"sink":"openpanel"|"glitchtip","attempts":n,
///    "queued":"<ISO UTC>","payload":{…}}
///
/// The queue lives in memory. New records are appended to the file at
/// once, so a crash or kill never loses what was queued; everything
/// else (sent, dropped, retried, trimmed) changes memory only, and
/// sync() rewrites the file — once per flush cycle and at quit, never
/// per request, so the GUI thread doesn't rewrite up to 512 KiB after
/// every POST.
///
/// open() takes <queueDir>/queue.lock for the whole session (stale
/// time 0: a live owner's lock never expires by age, as for main.cpp's
/// instance lock). Another live process holding it — main.cpp runs
/// unguarded when the instance lock can't be created — leaves this
/// queue unusable, and this process records and sends nothing.
///
/// Never logs payloads.
class EventQueue {
public:
    struct Limits {
        int maxQueued = 500;
        qint64 maxBytes = 512 * 1024;
        int maxAgeDays = 14;
        int maxAttempts = 5;
    };

    EventQueue(QString dir, Limits limits);
    ~EventQueue();   ///< releases the lock; does NOT sync

    EventQueue(const EventQueue&) = delete;
    EventQueue& operator=(const EventQueue&) = delete;

    /// Takes the lock and loads the file. Malformed lines, lines with
    /// v != 1, records older than maxAgeDays and records that used up
    /// maxAttempts are dropped (the next sync() writes that back);
    /// the caps are applied. False, and unusable, when the lock is
    /// held by another live process or can't be created.
    bool open();
    bool isUsable() const;

    bool isEmpty() const { return records_.isEmpty(); }
    qsizetype size() const { return records_.size(); }
    const QueuedRecord& front() const { return records_.first(); }
    const QList<QueuedRecord>& records() const { return records_; }

    /// Appends to memory and to the file, and gives the record its id.
    /// A crash record replaces the one already queued. Over maxQueued
    /// or maxBytes, the oldest OpenPanel records go first, then the
    /// oldest errors, the crash last (memory only; the next sync()
    /// trims the file).
    void append(QueuedRecord record);
    /// Removes the record with this id (sent, or refused for good).
    /// Nothing if the caps dropped it meanwhile.
    void remove(quint64 id);
    /// Counts a failed send of the record with this id; one that
    /// reaches maxAttempts is dropped. Nothing if it is gone.
    void bumpAttempts(quint64 id);

    /// Rewrites the file from memory (QSaveFile) if anything changed
    /// since the last write. An empty queue removes the file.
    bool sync();

    /// Releases the lock without writing anything.
    void close();

    QString filePath() const;
    static QString filePath(const QString& dir);

    /// Deletes the queue file and its lock, unless another live
    /// process holds the lock (its session is still using them). For
    /// the off gates at startup and the Settings switch.
    static void discard(const QString& dir);

    /// How many times any EventQueue in this process has written to
    /// disk (appends and rewrites). Tests use it to prove a path makes
    /// no write.
    static quint64 diskWrites();

private:
    void enforceCaps();
    void dropAt(qsizetype index);
    qsizetype indexOf(quint64 id) const;

    QString dir_;
    Limits limits_;
    std::unique_ptr<QLockFile> lock_;
    QList<QueuedRecord> records_;
    QList<qint64> lineBytes_;   // serialized size of each record, newline included
    qint64 totalBytes_ = 0;
    bool dirty_ = false;
    quint64 nextId_ = 1;
};

} // namespace gitbolt::telemetry
