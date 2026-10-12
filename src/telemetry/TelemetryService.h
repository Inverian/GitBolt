#pragma once

#include "telemetry/EventQueue.h"
#include "telemetry/EventSchema.h"
#include "telemetry/TelemetryConfig.h"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QTimer>

#include <functional>
#include <memory>

class QNetworkAccessManager;
class QNetworkReply;

namespace gitbolt::telemetry {

/// The telemetry client: owned by main(), created only when the gate is
/// Active or SettingOff (TelemetryGate.h), handed to MainWindow as a
/// raw pointer and declared before it, so it outlives the window.
///
/// Records go into a bounded queue next to GitBolt.ini (EventQueue);
/// one paced sender on the GUI thread drains it, one POST at a time:
///   - A flush starts firstFlushDelayMs after the first unsent record
///     (and after start() if records are left from earlier sessions).
///     A crash summary goes first (the queue keeps it in front), and
///     at once: start() flushes on the next event-loop turn.
///   - The next request starts paceMs after the previous one finished;
///     at most maxPerFlush requests per flushIntervalMs.
///   - A Retry outcome (Transport.h) counts an attempt and ends the
///     cycle: nothing more goes out until the next flushIntervalMs
///     tick, which is the only backoff. So an outage costs one request
///     per tick, and a poisoned record is gone after maxAttempts ticks.
///     Unreachable (offline: no server saw the request) ends the cycle
///     the same way but counts nothing.
///   - Command counts become command_used events every countWindowMs
///     and at quit.
///
/// Every record*, count* and flush entry point does nothing unless the
/// service is active: enabled, not quitting, its queue lock held. The
/// network stack is created at the first flush, so loading the TLS
/// backend never lands on the launch path. Nothing runs on the global
/// thread pool, whose waitForDone() main() blocks on at exit.
class TelemetryService : public QObject {
    Q_OBJECT
public:
    /// `installIdProvider` is called once, when the first OpenPanel
    /// event is built in an active session (SettingsService::
    /// ensureTelemetryInstallId), so inert and off sessions never
    /// create an ID. An ID that isn't a canonical UUID v4 is never
    /// sent; OpenPanel events are then dropped. `enabled`: the
    /// Settings switch (false for the SettingOff gate).
    ///
    /// `switchReader` re-reads the switch from disk (SettingsService::
    /// reloadUsageStatisticsEnabled), for a switch turned off in
    /// another process: no settingsChanged reaches this one, and while
    /// this process holds the queue lock the other can't delete the
    /// queue. Read at every flush cycle (each retry tick and each
    /// first flush) and at quit; false acts as setUserEnabled(false),
    /// before anything is sent or written.
    TelemetryService(Config config, std::function<QString()> installIdProvider, bool enabled,
                     std::function<bool()> switchReader = {}, QObject* parent = nullptr);
    ~TelemetryService() override;

    /// Starts the timers and schedules the first flush. main() calls it
    /// after window.show(), so nothing touches the network before the
    /// window is up.
    void start();

    /// app_launched, once per launch (main, after window.show()).
    void recordLaunch();
    /// Counts one trigger of a QAction by objectName (commandName()
    /// sanitizes; a rejected name is not counted).
    void countCommand(const QString& objectName);
    /// repo_opened: open and first-history-page durations (clamped),
    /// and the first page's size as a bucket.
    void recordRepoOpened(qint64 openMs, qint64 historyMs, int firstPageCommits);
    /// A GlitchTip error event; `detail` as glitchTipError() takes it.
    /// One per (kind, sanitized detail) per session, at most
    /// maxErrorsPerSession.
    void recordError(ErrorKind kind, const QString& detail = {});
    /// Parses a crash.log (CrashLogParser) and queues the reduced event
    /// if the parser accepts it. Synchronous and network-free: main()
    /// calls it right before the "Crash Detected" box deletes the file.
    /// True when a summary was queued: the box then says it will be
    /// sent.
    bool queueCrashReport(const QString& crashLog);

    /// The Settings switch; idempotent, so MainWindow can call it on
    /// every settingsChanged. Off, in this order and before control
    /// returns to the event loop: the in-flight request is aborted
    /// (its record is not touched), timers stop, counts are cleared
    /// (the error dedupe is kept, so "once per session" holds), the
    /// queue lock is released with nothing written, and the queue file
    /// is deleted. On again: the queue is
    /// reopened and sending resumes (the next app_launched comes with
    /// the next launch).
    void setUserEnabled(bool enabled);

    /// For QCoreApplication::aboutToQuit: writes pending counts into
    /// the queue, aborts the in-flight request (its record stays queued
    /// with its attempts unchanged), stops the timers and rewrites the
    /// queue once. Nothing is sent after this. A switch turned off in
    /// another process (`switchReader`) deletes the queue instead.
    void persistPending();

    bool isActive() const;

    /// Records waiting to be sent; 0 while inactive.
    qsizetype queuedCount() const;

private:
    void openQueue();
    QString installId();
    void enqueue(Sink sink, QJsonObject payload);
    void scheduleFlush();
    void beginCycle();
    void sendFront();
    void onReplyFinished(QNetworkReply* reply, quint64 recordId);
    void endCycle();
    void onRetryTick();
    void flushCounts();
    void abortInFlight();
    void stopTimers();
    bool switchedOffElsewhere();

    Config config_;
    std::function<QString()> installIdProvider_;
    std::function<bool()> switchReader_;
    ClientInfo client_;
    QString userAgent_;
    QString installId_;
    bool installIdFetched_ = false;
    bool complete_ = false;
    bool userEnabled_ = false;
    bool started_ = false;
    bool quitting_ = false;
    bool inCycle_ = false;
    bool backingOff_ = false;
    int sentThisTick_ = 0;

    std::unique_ptr<EventQueue> queue_;
    QNetworkAccessManager* network_ = nullptr;   // child, created at the first flush
    QNetworkReply* reply_ = nullptr;             // the one request in flight

    QTimer flushTimer_;   // single shot: firstFlushDelayMs after a record
    QTimer retryTimer_;   // flushIntervalMs: retries and the request budget
    QTimer paceTimer_;    // single shot: paceMs between requests
    QTimer countTimer_;   // countWindowMs: command counts out

    QHash<QString, int> counts_;
    QSet<QString> reportedErrors_;
};

} // namespace gitbolt::telemetry
