#pragma once

#include "telemetry/CrashLogParser.h"

#include <QDateTime>
#include <QJsonObject>
#include <QString>

#include <optional>

namespace gitbolt::telemetry {

/// The one place where event shapes and sanitizing live. Every builder
/// starts from an empty QJsonObject and inserts allowlisted keys only,
/// and none takes a free-form string that reaches the payload
/// unchecked: anything that could carry a path, a branch, a remote, a
/// commit, an email or git's own error text is either mapped onto a
/// fixed vocabulary or dropped. "Never sent" is guaranteed by how the
/// events are built, not by filtering afterwards.
///
/// OpenPanel gets one event per POST <host>/track:
///   {"type":"track","payload":{"name":…,"profileId":<install ID>,
///    "properties":{"app_version","__deviceId","__os","__osVersion",
///                  "__device", [__timestamp], …the event's own}}}
///   - app_launched: arch, qt_version, locale
///   - command_used: command, count
///   - repo_opened:  open_ms, history_ms, commits_bucket
/// GlitchTip gets Sentry store events (POST <host>/api/<id>/store/)
/// with no user, server_name, request, extra or breadcrumbs, and
/// neither the install ID nor the device ID: errors and crashes can't
/// be linked to an install.

enum class ErrorKind { OperationFailed, CommitFailed, RepoOpenFailed };

/// "operation_failed", "commit_failed", "repo_open_failed".
QString errorKindName(ErrorKind kind);

/// What every event says about the machine, captured once per session.
struct Environment {
    QString os;          ///< "macOS", "Windows" or "Linux"
    QString osVersion;   ///< "15.6", "11"; Linux: "<distro> <version>", e.g. "ubuntu 24.04"
    QString arch;        ///< QSysInfo::currentCpuArchitecture(), e.g. "arm64"
    QString qtVersion;   ///< qVersion(), the runtime
    QString locale;      ///< UI language only, e.g. "en" (no region)

    static Environment current();
};

/// The app's side of every event. GlitchTip builders take only this;
/// the install ID is a separate argument of the OpenPanel builders, so
/// it can't reach GlitchTip by accident.
struct ClientInfo {
    QString appVersion;
    Environment env;
};

/// "GitBolt/<version> (<OS token>)", e.g. "GitBolt/0.9.5 (Macintosh;
/// Mac OS X 15_6)". The token carries only OS family, version and CPU
/// architecture, all already sent as properties. Qt's default
/// "Mozilla/5.0", or a bare "GitBolt/0.9.5", gets the client classed as
/// a server by some OpenPanel versions, which then ignore __os and
/// __device; the token works whichever way the server reads it.
QString userAgent(const ClientInfo& client);

/// A random UUID v4 in its canonical lower-case 36-character form, as
/// SettingsService stores it. Anything else in GitBolt.ini (a hand
/// edit) is never sent as a profile ID.
bool isValidInstallId(const QString& installId);

/// OpenPanel's __deviceId: the first 32 hex digits of
/// SHA-256("gitbolt-device-id:" + installId). It must differ from the
/// profile ID (OpenPanel builds sessions from nullIf(profile_id,
/// device_id)); 32 hex digits never equal a hyphenated UUID. Derived,
/// so an erasure request quoting only the install ID reaches the
/// device rows too, and never stored.
QString deviceIdFor(const QString& installId);

/// A QAction objectName fit to send: it matches
/// ^[A-Za-z][A-Za-z0-9._-]{0,63}$, and "file.recent.<n>" (a Recent
/// Repositories entry) becomes "file.recent". Anything else: nullopt.
/// Names are fixed when the window is built (explicit ones like
/// "act.commit", menu-path fallbacks); bridge slugs and action texts,
/// which can hold paths and branch names, must never come here.
std::optional<QString> commandName(const QString& objectName);

/// GitService's fixed operation literal in snake_case ("stage hunk" →
/// "stage_hunk", "rebase --continue" → "rebase_continue", "cherryPick"
/// → "cherry_pick") when it is on the allowlist of the literals
/// GitService emits; anything else is "other". Never the error text.
QString operationName(const QString& opLiteral);

/// git::GitErrorCode by value → "not_found", "locked", …; unknown
/// values → "other". (This library doesn't link git; a test pins the
/// values against the enum.)
QString openErrorName(int gitErrorCode);

/// "0", "1-99", "100-255" or "256+": the first history page holds at
/// most 256 commits.
QString commitsBucket(int firstPageCommits);

/// Durations are clamped to [0, 600000] ms.
qint64 clampDurationMs(qint64 ms);

// OpenPanel ----------------------------------------------------------------

// Each is nullopt when the install ID fails isValidInstallId().

std::optional<QJsonObject> appLaunchedEvent(const ClientInfo& client, const QString& installId);
/// Also nullopt when commandName() rejects the name or count < 1.
std::optional<QJsonObject> commandUsedEvent(const ClientInfo& client, const QString& installId,
                                            const QString& objectName, int count);
std::optional<QJsonObject> repoOpenedEvent(const ClientInfo& client, const QString& installId,
                                           qint64 openMs, qint64 historyMs,
                                           int firstPageCommits);

/// The track event with properties.__timestamp set to `queued`. Only
/// for a record older than Config::timestampAfterMs at send time.
QJsonObject withTimestamp(QJsonObject event, const QDateTime& queued);

// GlitchTip ----------------------------------------------------------------

/// level "error"; `detail` goes through operationName() for
/// OperationFailed and must be an openErrorName() value for
/// RepoOpenFailed (else "other"); CommitFailed ignores it.
QJsonObject glitchTipError(const ClientInfo& client, ErrorKind kind, const QString& detail,
                           const QDateTime& at);

/// level "fatal", from a parsed report. Contexts and release come from
/// the report, not the running app.
QJsonObject glitchTipCrash(const CrashEvent& crash);

/// The sanitized detail recordError() dedupes on: operationName(),
/// the checked openErrorName() value, or empty.
QString errorDetail(ErrorKind kind, const QString& detail);

} // namespace gitbolt::telemetry
