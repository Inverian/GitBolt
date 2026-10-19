#include "telemetry/EventSchema.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QLocale>
#include <QRegularExpression>
#include <QSet>
#include <QSysInfo>
#include <QTimeZone>
#include <QUuid>

#include <algorithm>

namespace gitbolt::telemetry {

namespace {

QString checked(const QString& value, const QRegularExpression& pattern,
                const QString& fallback) {
    return pattern.match(value).hasMatch() ? value : fallback;
}

QString isoUtc(const QDateTime& at) {
    return at.toTimeZone(QTimeZone::UTC).toString(Qt::ISODateWithMs);
}

std::optional<QJsonObject> trackEvent(const ClientInfo& client, const QString& installId,
                                      const QString& name, QJsonObject properties) {
    if (!isValidInstallId(installId))
        return std::nullopt;
    properties.insert(QStringLiteral("app_version"), client.appVersion);
    properties.insert(QStringLiteral("__deviceId"), deviceIdFor(installId));
    properties.insert(QStringLiteral("__os"), client.env.os);
    properties.insert(QStringLiteral("__osVersion"), client.env.osVersion);
    properties.insert(QStringLiteral("__device"), QStringLiteral("desktop"));

    QJsonObject payload;
    payload.insert(QStringLiteral("name"), name);
    payload.insert(QStringLiteral("profileId"), installId);
    payload.insert(QStringLiteral("properties"), properties);
    QJsonObject event;
    event.insert(QStringLiteral("type"), QStringLiteral("track"));
    event.insert(QStringLiteral("payload"), payload);
    return event;
}

QJsonObject contexts(const QString& os, const QString& osVersion, const QString& arch,
                     const QString& qtVersion) {
    QJsonObject osContext;
    osContext.insert(QStringLiteral("name"), os);
    if (!osVersion.isEmpty())
        osContext.insert(QStringLiteral("version"), osVersion);
    QJsonObject result;
    result.insert(QStringLiteral("os"), osContext);
    if (!arch.isEmpty())
        result.insert(QStringLiteral("device"), QJsonObject{{QStringLiteral("arch"), arch}});
    QJsonObject runtime;
    runtime.insert(QStringLiteral("name"), QStringLiteral("Qt"));
    if (!qtVersion.isEmpty())
        runtime.insert(QStringLiteral("version"), qtVersion);
    result.insert(QStringLiteral("runtime"), runtime);
    return result;
}

// The keys every GlitchTip event carries; the caller adds level,
// message or exception, fingerprint and tags.
QJsonObject sentryEvent(const QString& level, const QString& release, const QDateTime& at,
                        const QJsonObject& eventContexts) {
    QJsonObject event;
    event.insert(QStringLiteral("event_id"), QUuid::createUuid().toString(QUuid::Id128));
    event.insert(QStringLiteral("timestamp"), isoUtc(at));
    event.insert(QStringLiteral("platform"), QStringLiteral("native"));
    event.insert(QStringLiteral("logger"), QStringLiteral("gitbolt"));
    event.insert(QStringLiteral("level"), level);
    event.insert(QStringLiteral("release"), QStringLiteral("gitbolt@%1").arg(release));
    event.insert(QStringLiteral("environment"), QStringLiteral("production"));
    event.insert(QStringLiteral("contexts"), eventContexts);
    return event;
}

QString hexAddress(quint64 value) {
    return QStringLiteral("0x%1").arg(value, 0, 16);
}

} // namespace

QString errorKindName(ErrorKind kind) {
    switch (kind) {
    case ErrorKind::OperationFailed: return QStringLiteral("operation_failed");
    case ErrorKind::CommitFailed:    return QStringLiteral("commit_failed");
    case ErrorKind::RepoOpenFailed:  return QStringLiteral("repo_open_failed");
    }
    return QStringLiteral("other");
}

Environment Environment::current() {
    static const QRegularExpression versionPattern(QStringLiteral("^[A-Za-z0-9 ._-]{1,40}$"));
    static const QRegularExpression archPattern(QStringLiteral("^[a-z0-9_]{1,16}$"));
    static const QRegularExpression qtPattern(QStringLiteral("^\\d+\\.\\d+\\.\\d+$"));
    static const QRegularExpression languagePattern(QStringLiteral("^[a-z]{2,3}$"));

    Environment env;
#if defined(Q_OS_MACOS)
    env.os = QStringLiteral("macOS");
    env.osVersion = QSysInfo::productVersion();
#elif defined(Q_OS_WIN)
    env.os = QStringLiteral("Windows");
    env.osVersion = QSysInfo::productVersion();
#else
    env.os = QStringLiteral("Linux");
    env.osVersion = QSysInfo::productType() + QLatin1Char(' ') + QSysInfo::productVersion();
#endif
    env.osVersion = checked(env.osVersion, versionPattern, QStringLiteral("unknown"));
    env.arch = checked(QSysInfo::currentCpuArchitecture(), archPattern, QStringLiteral("unknown"));
    env.qtVersion = checked(QString::fromLatin1(qVersion()), qtPattern, QStringLiteral("unknown"));
    env.locale = checked(QLocale::languageToCode(QLocale::system().language()), languagePattern,
                         QStringLiteral("und"));
    return env;
}

QString userAgent(const ClientInfo& client) {
    static const QRegularExpression dotted(QStringLiteral("^\\d+(\\.\\d+)*$"));
    const Environment& env = client.env;
    QString token;
    if (env.os == QLatin1String("macOS")) {
        token = QStringLiteral("Macintosh");
        if (dotted.match(env.osVersion).hasMatch())
            token += QStringLiteral("; Mac OS X ") + QString(env.osVersion).replace(u'.', u'_');
    } else if (env.os == QLatin1String("Windows")) {
        token = env.arch == QLatin1String("arm64") ? QStringLiteral("Windows NT 10.0; ARM64")
                                                   : QStringLiteral("Windows NT 10.0; Win64; x64");
    } else {
        token = QStringLiteral("X11; Linux ")
                + (env.arch == QLatin1String("arm64") ? QStringLiteral("aarch64") : env.arch);
    }
    return QStringLiteral("GitBolt/%1 (%2)").arg(client.appVersion, token);
}

bool isValidInstallId(const QString& installId) {
    static const QRegularExpression uuidV4(QStringLiteral(
        "^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$"));
    return uuidV4.match(installId).hasMatch();
}

QString deviceIdFor(const QString& installId) {
    const QByteArray digest = QCryptographicHash::hash(
        QByteArrayLiteral("gitbolt-device-id:") + installId.toUtf8(), QCryptographicHash::Sha256);
    return QString::fromLatin1(digest.toHex().left(32));
}

std::optional<QString> commandName(const QString& objectName) {
    static const QRegularExpression name(QStringLiteral("^[A-Za-z][A-Za-z0-9._-]{0,63}$"));
    // Recent Repositories entries are numbered by position; one name
    // for all of them (their texts are paths and never come here).
    if (objectName.startsWith(QLatin1String("file.recent.")))
        return QStringLiteral("file.recent");
    if (!name.match(objectName).hasMatch())
        return std::nullopt;
    return objectName;
}

QString operationName(const QString& opLiteral) {
    // Every literal GitService passes to operationFailed, normalized.
    static const QSet<QString> known = {
        QStringLiteral("abort"),            QStringLiteral("add_worktree"),
        QStringLiteral("autostash"),        QStringLiteral("blame"),
        QStringLiteral("checkout"),         QStringLiteral("cherry_pick"),
        QStringLiteral("cherry_pick_abort"),
        QStringLiteral("cherry_pick_continue"), QStringLiteral("cherry_pick_skip"),
        QStringLiteral("conflicts"),
        QStringLiteral("create_branch"),    QStringLiteral("create_tag"),
        QStringLiteral("delete_branch"),    QStringLiteral("delete_remote_branch"),
        QStringLiteral("delete_tag"),       QStringLiteral("discard_file"),
        QStringLiteral("feature_finish"),   QStringLiteral("feature_start"),
        QStringLiteral("fetch"),            QStringLiteral("fsck"),
        QStringLiteral("gc"),               QStringLiteral("git_flow_init"),
        QStringLiteral("hotfix_finish"),    QStringLiteral("hotfix_start"),
        QStringLiteral("lock_worktree"),    QStringLiteral("merge_abort"),
        QStringLiteral("prune"),
        QStringLiteral("pull"),             QStringLiteral("push"),
        QStringLiteral("rebase"),           QStringLiteral("rebase_abort"),
        QStringLiteral("rebase_continue"),  QStringLiteral("rebase_skip"),
        QStringLiteral("release_finish"),   QStringLiteral("release_start"),
        QStringLiteral("remove_worktree"),  QStringLiteral("repack"),
        QStringLiteral("resolve_conflicts"), QStringLiteral("revert_abort"),
        QStringLiteral("revert_continue"),
        QStringLiteral("revert_skip"),      QStringLiteral("stage"),
        QStringLiteral("stage_all"),        QStringLiteral("stage_hunk"),
        QStringLiteral("stash_apply"),      QStringLiteral("stash_drop"),
        QStringLiteral("stash_pop"),        QStringLiteral("stash_save"),
        QStringLiteral("status"),
        QStringLiteral("submodule_init"),   QStringLiteral("submodule_update"),
        QStringLiteral("unlock_worktree"),  QStringLiteral("unstage"),
        QStringLiteral("unstage_all"),
    };
    QString snake;
    for (qsizetype i = 0; i < opLiteral.size() && snake.size() <= 64; ++i) {
        const QChar c = opLiteral.at(i);
        if (c.isUpper() && i > 0 && opLiteral.at(i - 1).isLower())
            snake += QLatin1Char('_');
        if (c.isLetterOrNumber() && c.unicode() < 128)
            snake += c.toLower();
        else if (!snake.endsWith(QLatin1Char('_')))
            snake += QLatin1Char('_');
    }
    while (snake.startsWith(QLatin1Char('_')))
        snake.remove(0, 1);
    while (snake.endsWith(QLatin1Char('_')))
        snake.chop(1);
    return known.contains(snake) ? snake : QStringLiteral("other");
}

QString openErrorName(int gitErrorCode) {
    // git::GitErrorCode in declaration order (src/git/Error.h); Ok (0)
    // is not a failure.
    static const char* const names[] = {
        nullptr,          "generic_error",  "not_found",       "exists",
        "ambiguous",      "buffer_too_short", "user",          "bare_repo",
        "unborn_branch",  "unmerged",       "non_fast_forward", "invalid_spec",
        "conflict",       "locked",         "modified",        "auth",
        "certificate",    "applied",        "peel",            "eof",
        "invalid",        "uncommitted",    "directory",       "merge_conflict",
        "process_failed", "checkout_failed",
    };
    constexpr int count = int(sizeof(names) / sizeof(names[0]));
    if (gitErrorCode <= 0 || gitErrorCode >= count)
        return QStringLiteral("other");
    return QString::fromLatin1(names[gitErrorCode]);
}

QString commitsBucket(int firstPageCommits) {
    if (firstPageCommits <= 0)
        return QStringLiteral("0");
    if (firstPageCommits < 100)
        return QStringLiteral("1-99");
    if (firstPageCommits < 256)
        return QStringLiteral("100-255");
    return QStringLiteral("256+");
}

qint64 clampDurationMs(qint64 ms) {
    return std::clamp<qint64>(ms, 0, 600000);
}

std::optional<QJsonObject> appLaunchedEvent(const ClientInfo& client, const QString& installId) {
    QJsonObject properties;
    properties.insert(QStringLiteral("arch"), client.env.arch);
    properties.insert(QStringLiteral("qt_version"), client.env.qtVersion);
    properties.insert(QStringLiteral("locale"), client.env.locale);
    return trackEvent(client, installId, QStringLiteral("app_launched"), properties);
}

std::optional<QJsonObject> commandUsedEvent(const ClientInfo& client, const QString& installId,
                                            const QString& objectName, int count) {
    const std::optional<QString> command = commandName(objectName);
    if (!command || count < 1)
        return std::nullopt;
    QJsonObject properties;
    properties.insert(QStringLiteral("command"), *command);
    properties.insert(QStringLiteral("count"), count);
    return trackEvent(client, installId, QStringLiteral("command_used"), properties);
}

std::optional<QJsonObject> repoOpenedEvent(const ClientInfo& client, const QString& installId,
                                           qint64 openMs, qint64 historyMs,
                                           int firstPageCommits) {
    QJsonObject properties;
    properties.insert(QStringLiteral("open_ms"), clampDurationMs(openMs));
    properties.insert(QStringLiteral("history_ms"), clampDurationMs(historyMs));
    properties.insert(QStringLiteral("commits_bucket"), commitsBucket(firstPageCommits));
    return trackEvent(client, installId, QStringLiteral("repo_opened"), properties);
}

QJsonObject withTimestamp(QJsonObject event, const QDateTime& queued) {
    QJsonObject payload = event.value(QStringLiteral("payload")).toObject();
    QJsonObject properties = payload.value(QStringLiteral("properties")).toObject();
    properties.insert(QStringLiteral("__timestamp"), isoUtc(queued));
    payload.insert(QStringLiteral("properties"), properties);
    event.insert(QStringLiteral("payload"), payload);
    return event;
}

QString errorDetail(ErrorKind kind, const QString& detail) {
    switch (kind) {
    case ErrorKind::OperationFailed:
        return operationName(detail);
    case ErrorKind::RepoOpenFailed:
        for (int code = 1; code < 64; ++code) {
            const QString name = openErrorName(code);
            if (name == QLatin1String("other"))
                break;
            if (name == detail)
                return name;
        }
        return QStringLiteral("other");
    case ErrorKind::CommitFailed:
        return {};
    }
    return {};
}

QJsonObject glitchTipError(const ClientInfo& client, ErrorKind kind, const QString& detail,
                           const QDateTime& at) {
    const QString kindName = errorKindName(kind);
    const QString clean = errorDetail(kind, detail);

    QJsonObject event =
        sentryEvent(QStringLiteral("error"), client.appVersion, at,
                    contexts(client.env.os, client.env.osVersion, client.env.arch,
                             client.env.qtVersion));
    QJsonObject tags;
    tags.insert(QStringLiteral("kind"), kindName);
    QJsonArray fingerprint{kindName};
    if (kind == ErrorKind::OperationFailed)
        tags.insert(QStringLiteral("op"), clean);
    else if (kind == ErrorKind::RepoOpenFailed)
        tags.insert(QStringLiteral("code"), clean);
    if (!clean.isEmpty())
        fingerprint.append(clean);
    event.insert(QStringLiteral("message"),
                 clean.isEmpty() ? kindName : kindName + QStringLiteral(": ") + clean);
    event.insert(QStringLiteral("fingerprint"), fingerprint);
    event.insert(QStringLiteral("tags"), tags);
    return event;
}

QJsonObject glitchTipCrash(const CrashEvent& crash) {
    QJsonObject event = sentryEvent(
        QStringLiteral("fatal"), crash.release, crash.at,
        contexts(crash.os, crash.osVersion, crash.arch, crash.qtVersion));

    QJsonArray frames;
    for (const CrashFrame& f : crash.frames) {
        QJsonObject frame;
        frame.insert(QStringLiteral("package"), f.package);
        if (!f.function.isEmpty())
            frame.insert(QStringLiteral("function"), f.function);
        if (f.offset) {
            // Module-relative, with the image placed at 0: what an
            // offline symbolizer needs, and no ASLR base.
            frame.insert(QStringLiteral("instruction_addr"), hexAddress(*f.offset));
            frame.insert(QStringLiteral("image_addr"), QStringLiteral("0x0"));
        }
        frame.insert(QStringLiteral("in_app"), f.inApp);
        frames.append(frame);
    }

    QJsonObject signalInfo;
    signalInfo.insert(QStringLiteral("number"), crash.signalNumber);
    signalInfo.insert(QStringLiteral("name"), crash.signal);
    QJsonObject mechanism;
    mechanism.insert(QStringLiteral("type"), QStringLiteral("signalhandler"));
    mechanism.insert(QStringLiteral("handled"), false);
    mechanism.insert(QStringLiteral("meta"), QJsonObject{{QStringLiteral("signal"), signalInfo}});

    QJsonObject exception;
    exception.insert(QStringLiteral("type"), crash.signal);
    exception.insert(QStringLiteral("value"), crash.signal);
    exception.insert(QStringLiteral("mechanism"), mechanism);
    exception.insert(QStringLiteral("stacktrace"),
                     QJsonObject{{QStringLiteral("frames"), frames}});

    event.insert(QStringLiteral("exception"),
                 QJsonObject{{QStringLiteral("values"), QJsonArray{exception}}});
    event.insert(QStringLiteral("fingerprint"), QJsonArray::fromStringList(crash.fingerprint));
    event.insert(QStringLiteral("tags"),
                 QJsonObject{{QStringLiteral("kind"), QStringLiteral("crash")}});
    return event;
}

} // namespace gitbolt::telemetry
