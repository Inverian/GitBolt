#pragma once

#include <QString>
#include <QUrl>

namespace gitbolt::telemetry {

/// Everything the telemetry client needs from outside: where to send,
/// with which keys, which version it reports, where the queue lives,
/// and the pacing and caps. A plain struct so this library never
/// depends on conf or app: main() fills it from the endpoints compiled
/// into the app target (only there, so no library or test binary can
/// carry them), and the tests fill it with a loopback stub and short
/// timings.
struct Config {
    QUrl trackUrl;              ///< OpenPanel: <host>/track
    QString openPanelClientId;
    QString openPanelSecret;
    QUrl storeUrl;              ///< GlitchTip: <host>/api/<project>/store/
    QString glitchTipKey;
    QString appVersion;         ///< GITBOLT_VERSION, e.g. "0.9.5"
    QString queueDir;           ///< <dir of GitBolt.ini>/telemetry

    /// Gap between the end of one request and the start of the next.
    int paceMs = 1000;
    /// Delay from the first unsent record to the flush that sends it.
    /// Short, because OpenPanel drops the device and session of any
    /// event that arrives more than 15 minutes after its __timestamp.
    int firstFlushDelayMs = 10000;
    /// Retry cadence: after a failed request nothing is sent until the
    /// next tick, and each tick renews the per-tick request budget.
    int flushIntervalMs = 15 * 60 * 1000;
    /// Command counts go out as command_used every this often (and at
    /// quit), so they reach the server while the session is live.
    int countWindowMs = 10 * 60 * 1000;
    int transferTimeoutMs = 10000;
    /// Requests per flushIntervalMs, across all flushes in it.
    int maxPerFlush = 60;
    /// A record that failed this often is dropped.
    int maxAttempts = 5;
    int maxQueued = 500;
    qint64 maxQueueBytes = 512 * 1024;
    int maxAgeDays = 14;
    int maxErrorsPerSession = 20;
    /// __timestamp is sent only for a record queued longer ago than
    /// this (client clock on both ends, so skew cancels out). A fresh
    /// event takes the server's receipt time, which a slow client
    /// clock can't push past OpenPanel's 15-minute "past event" line.
    qint64 timestampAfterMs = 60 * 1000;

    /// Builds the two ingest URLs from the origin (scheme://host[:port],
    /// no path) and the GlitchTip project id.
    static Config fromEndpoints(const QString& host, const QString& clientId,
                                const QString& clientSecret, const QString& dsnKey,
                                const QString& project, const QString& version,
                                const QString& dir);

    /// True only when every field is filled in, the version is a
    /// release version, and both URLs are https, or plain http to
    /// 127.0.0.1 or [::1] (the tests' stub, or a developer's local
    /// stub build). Anything else counts as compiled out: main() then
    /// creates no service at all.
    bool isComplete() const;

    /// https, or http to a loopback address; no user info.
    static bool isAllowedUrl(const QUrl& url);
};

} // namespace gitbolt::telemetry
