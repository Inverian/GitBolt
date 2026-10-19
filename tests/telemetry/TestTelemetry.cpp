//
// TestTelemetry — the telemetry client against a loopback HTTP stub
// (HttpStub.h). Never a real endpoint: test binaries don't link
// src/app, the only place endpoints are compiled into.
//
//   - the gate table, injected environment only (ctest and CI set
//     DO_NOT_TRACK, GITBOLT_TELEMETRY, QT_QPA_PLATFORM and CI in the
//     real one), and the shared test-session / opt-out predicates
//   - what each event looks like on the wire: payload keys, the exact
//     header set (HttpStub fails on any extra header), User-Agent,
//     __deviceId, and no __timestamp on fresh events
//   - pacing, the per-tick budget, the debounced first flush (a crash
//     summary goes first, at once), retry and drop outcomes, backoff
//     until the next tick, offline tries that cost no attempt
//   - the off switch, here or in another process, and quit while a
//     request is in flight: nothing written before the discard,
//     attempts untouched, nothing lost
//   - the queue's caps, load-time rules and lock, and a stale lock no
//     PID check can prove stale
//   - sanitizers: paths, branches, emails and git's error text never
//     leave
//

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QNetworkAccessManager>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>
#include <QUuid>

#include "CrashHeader.h"
#include "HttpStub.h"
#include "git/Error.h"
#include "telemetry/EventQueue.h"
#include "telemetry/EventSchema.h"
#include "telemetry/TelemetryConfig.h"
#include "telemetry/TelemetryGate.h"
#include "telemetry/TelemetryService.h"
#include "telemetry/Transport.h"
#include "util/TestSession.h"

#include <memory>
#include <optional>

using namespace gitbolt::telemetry;
using gitbolt::test::HttpStub;
using gitbolt::test::StubRequest;
using gitbolt::test::StubResponse;

using EnvMap = QHash<QString, QString>;
Q_DECLARE_METATYPE(gitbolt::telemetry::Outcome)

namespace {

const QString kInstallId = QStringLiteral("1b4e28ba-2fa1-4d2c-883f-0016d3cca427");
const QString kGlitchTipKey = QStringLiteral("0123456789abcdef0123456789abcdef");

gitbolt::util::EnvLookup envFrom(const EnvMap& env) {
    return [env](const char* name) -> std::optional<QString> {
        const auto it = env.constFind(QString::fromLatin1(name));
        if (it == env.constEnd())
            return std::nullopt;
        return *it;
    };
}

// Short timings, so a test takes well under a second; a retry tick
// long enough not to fire unless a test shortens it.
Config testConfig(const HttpStub& stub, const QString& dir) {
    Config config = Config::fromEndpoints(stub.origin(), QStringLiteral("test-client"),
                                          QStringLiteral("test-key-value"), kGlitchTipKey,
                                          QStringLiteral("42"), QStringLiteral("1.2.3"),
                                          dir + QStringLiteral("/telemetry"));
    config.paceMs = 50;
    config.transferTimeoutMs = 300;
    config.firstFlushDelayMs = 0;
    config.maxPerFlush = 5;
    config.maxAttempts = 3;
    config.flushIntervalMs = 60000;
    config.countWindowMs = 60000;
    return config;
}

struct Provider {
    int calls = 0;
    QString id = kInstallId;
    std::function<QString()> fn() {
        return [this] {
            ++calls;
            return id;
        };
    }
};

QList<QJsonObject> queueLines(const Config& config) {
    QList<QJsonObject> lines;
    QFile file(EventQueue::filePath(config.queueDir));
    if (!file.open(QIODevice::ReadOnly))
        return lines;
    while (!file.atEnd()) {
        const QByteArray line = file.readLine().trimmed();
        if (!line.isEmpty())
            lines << QJsonDocument::fromJson(line).object();
    }
    return lines;
}

QJsonObject properties(const StubRequest& request) {
    return request.json()
        .value(QStringLiteral("payload")).toObject()
        .value(QStringLiteral("properties")).toObject();
}

QSet<QString> keySet(const QJsonObject& object) {
    const QStringList keys = object.keys();
    return QSet<QString>(keys.cbegin(), keys.cend());
}

ClientInfo clientFor(const QString& os, const QString& osVersion, const QString& arch) {
    ClientInfo client;
    client.appVersion = QStringLiteral("0.9.5");
    client.env.os = os;
    client.env.osVersion = osVersion;
    client.env.arch = arch;
    client.env.qtVersion = QStringLiteral("6.10.0");
    client.env.locale = QStringLiteral("en");
    return client;
}

int closedPort() {
    QTcpServer probe;
    probe.listen(QHostAddress::LocalHost, 0);
    const int port = probe.serverPort();
    probe.close();
    return port;
}

// A crash.log the parser accepts. A macOS report wherever this runs:
// the parser's frame rules follow the OS line (CrashHeader.h).
QString crashReport() {
    return QString::fromUtf8(gitbolt::test::crashHeader("0.9.1", true, false, false))
           + QStringLiteral("Signal: SIGSEGV (11)\nEpoch: 1760000000\n\n"
                            "--- Stack Trace ---\n"
                            "#0 GitBolt +0x10 ?\n"
                            "#1 libsystem_platform.dylib +0x20 _sigtramp+0x1\n"
                            "#2 GitBolt +0x30 ?\n"
                            "#3 dyld +0x40 start+0x2\n"
                            "\n=== End of Report ===\n");
}

// A new queue in `dir` holding one record.
bool writeOneRecordQueue(const QString& dir) {
    if (!QDir().mkpath(dir))
        return false;
    QFile file(EventQueue::filePath(dir));
    const QByteArray line =
        "{\"v\":1,\"sink\":\"openpanel\",\"attempts\":0,\"queued\":\""
        + QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs).toUtf8()
        + "\",\"payload\":{\"n\":1}}\n";
    return file.open(QIODevice::WriteOnly) && file.write(line) == line.size();
}

// Rewrites `path` with `content`, last modified a minute ago: long past
// util::removeUnprovableStaleLock's settle time.
bool writeAgedFile(const QString& path, const QByteArray& content) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
           && file.write(content) == content.size() && file.flush()
           && file.setFileTime(QDateTime::currentDateTime().addSecs(-60),
                               QFileDevice::FileModificationTime);
}

} // namespace

class TestTelemetry : public QObject {
    Q_OBJECT

private:
    std::unique_ptr<HttpStub> stub_;
    std::unique_ptr<QTemporaryDir> dir_;

    std::unique_ptr<TelemetryService> makeService(const Config& config, Provider& provider,
                                                  bool enabled = true) {
        return std::make_unique<TelemetryService>(config, provider.fn(), enabled);
    }

private slots:
    void init() {
        stub_ = std::make_unique<HttpStub>();
        QVERIFY(stub_->isListening());
        dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(dir_->isValid());
    }

    // The stub saw only the headers each sink may send, on the paths
    // the sinks use.
    void cleanup() {
        const QStringList violations = stub_->headerViolations();
        stub_.reset();
        dir_.reset();
        QVERIFY2(violations.isEmpty(), qPrintable(violations.join(QLatin1Char('\n'))));
    }

    // ---- gate ---------------------------------------------------------

    void gate_data() {
        QTest::addColumn<bool>("configured");
        QTest::addColumn<EnvMap>("env");
        QTest::addColumn<QString>("platform");
        QTest::addColumn<QStringList>("arguments");
        QTest::addColumn<bool>("settingOn");
        QTest::addColumn<QString>("expected");

        const QStringList noArgs{QStringLiteral("gitbolt")};
        auto row = [&](const char* name, bool configured, EnvMap env, const QString& platform,
                       bool settingOn, const char* expected, QStringList args = {}) {
            QTest::newRow(name) << configured << env << platform
                                << (args.isEmpty() ? noArgs : args) << settingOn
                                << QString::fromLatin1(expected);
        };
        const QString cocoa = QStringLiteral("cocoa");

        row("no endpoints beats everything", false,
            {{"GITBOLT_TEST_BRIDGE", "b"}, {"DO_NOT_TRACK", "1"}}, cocoa, true, "compiled-out");
        row("plain user session", true, {}, cocoa, true, "on");
        row("setting off", true, {}, cocoa, false, "off:setting");

        row("bridge", true, {{"GITBOLT_TEST_BRIDGE", "gbx"}}, cocoa, true, "inert:test-bridge");
        // main.cpp starts the bridge whenever the variable is SET; an
        // empty value picks the default socket.
        row("empty bridge is still the bridge", true, {{"GITBOLT_TEST_BRIDGE", ""}}, cocoa, true,
            "inert:test-bridge");
        row("bridge beats DO_NOT_TRACK", true,
            {{"GITBOLT_TEST_BRIDGE", "b"}, {"DO_NOT_TRACK", "1"}}, cocoa, true,
            "inert:test-bridge");
        row("bridge beats instance", true,
            {{"GITBOLT_TEST_BRIDGE", "b"}, {"GITBOLT_INSTANCE_NAME", "i"}}, cocoa, true,
            "inert:test-bridge");

        row("instance name", true, {{"GITBOLT_INSTANCE_NAME", "e2e-1"}}, cocoa, true,
            "inert:instance-name");
        row("empty instance name is unset", true, {{"GITBOLT_INSTANCE_NAME", ""}}, cocoa, true,
            "on");
        row("instance beats headless", true, {{"GITBOLT_INSTANCE_NAME", "i"}},
            QStringLiteral("offscreen"), true, "inert:instance-name");

        row("offscreen platform", true, {}, QStringLiteral("offscreen"), true, "inert:headless");
        row("minimal platform", true, {}, QStringLiteral("minimal"), true, "inert:headless");
        row("QT_QPA_PLATFORM offscreen", true, {{"QT_QPA_PLATFORM", "offscreen"}}, QString(),
            true, "inert:headless");
        row("QT_QPA_PLATFORM with options", true,
            {{"QT_QPA_PLATFORM", "offscreen:fontengine=freetype"}}, QString(), true,
            "inert:headless");
        row("argv -platform minimal", true, {}, QString(), true, "inert:headless",
            {QStringLiteral("gitbolt"), QStringLiteral("-platform"), QStringLiteral("minimal")});
        row("argv --platform offscreen", true, {}, QString(), true, "inert:headless",
            {QStringLiteral("gitbolt"), QStringLiteral("--platform"),
             QStringLiteral("offscreen")});
        row("argv repo named offscreen", true, {}, cocoa, true, "on",
            {QStringLiteral("gitbolt"), QStringLiteral("offscreen")});
        row("headless beats CI", true, {{"CI", "true"}}, QStringLiteral("offscreen"), true,
            "inert:headless");

        row("CI", true, {{"CI", "true"}}, cocoa, true, "inert:ci");
        row("GITHUB_ACTIONS", true, {{"GITHUB_ACTIONS", "true"}}, cocoa, true, "inert:ci");
        row("CI empty is unset", true, {{"CI", ""}}, cocoa, true, "on");
        row("CI=false", true, {{"CI", "false"}}, cocoa, true, "on");
        row("CI=0", true, {{"CI", "0"}}, cocoa, true, "on");
        row("CI beats DO_NOT_TRACK", true, {{"CI", "1"}, {"DO_NOT_TRACK", "1"}}, cocoa, true,
            "inert:ci");

        row("DO_NOT_TRACK=1", true, {{"DO_NOT_TRACK", "1"}}, cocoa, true, "off:do-not-track");
        row("DO_NOT_TRACK=yes", true, {{"DO_NOT_TRACK", "yes"}}, cocoa, true,
            "off:do-not-track");
        row("DO_NOT_TRACK=0 doesn't opt out", true, {{"DO_NOT_TRACK", "0"}}, cocoa, true, "on");
        row("DO_NOT_TRACK=false doesn't opt out", true, {{"DO_NOT_TRACK", "false"}}, cocoa, true,
            "on");
        row("DO_NOT_TRACK empty is unset", true, {{"DO_NOT_TRACK", ""}}, cocoa, true, "on");
        row("DO_NOT_TRACK beats the setting", true, {{"DO_NOT_TRACK", "1"}}, cocoa, false,
            "off:do-not-track");
        row("DO_NOT_TRACK beats GITBOLT_TELEMETRY", true,
            {{"DO_NOT_TRACK", "1"}, {"GITBOLT_TELEMETRY", "0"}}, cocoa, true, "off:do-not-track");

        row("GITBOLT_TELEMETRY=0", true, {{"GITBOLT_TELEMETRY", "0"}}, cocoa, true, "off:env");
        row("GITBOLT_TELEMETRY=FALSE", true, {{"GITBOLT_TELEMETRY", "FALSE"}}, cocoa, true,
            "off:env");
        row("GITBOLT_TELEMETRY=off", true, {{"GITBOLT_TELEMETRY", "off"}}, cocoa, true, "off:env");
        row("GITBOLT_TELEMETRY=No", true, {{"GITBOLT_TELEMETRY", "No"}}, cocoa, true, "off:env");
        row("GITBOLT_TELEMETRY=1", true, {{"GITBOLT_TELEMETRY", "1"}}, cocoa, true, "on");
        row("GITBOLT_TELEMETRY empty", true, {{"GITBOLT_TELEMETRY", ""}}, cocoa, true, "on");
        row("GITBOLT_TELEMETRY beats the setting", true, {{"GITBOLT_TELEMETRY", "0"}}, cocoa,
            false, "off:env");
    }

    void gate() {
        QFETCH(bool, configured);
        QFETCH(EnvMap, env);
        QFETCH(QString, platform);
        QFETCH(QStringList, arguments);
        QFETCH(bool, settingOn);
        QFETCH(QString, expected);

        GateInputs inputs;
        inputs.configured = configured;
        inputs.platformName = platform;
        inputs.arguments = arguments;
        inputs.settingOn = settingOn;
        inputs.env = envFrom(env);
        const Gate gate = evaluateGate(inputs);
        QCOMPARE(gateName(gate), expected);

        // The crash handler stamps "Session: test" on exactly the
        // sessions the gate keeps inert (one shared predicate), and
        // "Opt-out: env" on the environment opt-outs.
        if (configured) {
            const bool inert = gateName(gate).startsWith(QLatin1String("inert:"));
            QCOMPARE(gitbolt::util::isTestSession(
                         gitbolt::util::detectTestSession(platform, arguments, inputs.env)),
                     inert);
            if (!inert) {
                const bool envOff = gate == Gate::DoNotTrack || gate == Gate::EnvOff;
                QCOMPARE(gitbolt::util::isEnvOptOut(gitbolt::util::detectEnvOptOut(inputs.env)),
                         envOff);
            }
        }
    }

    void gateServiceAndDiscard() {
        struct Expect {
            Gate gate;
            bool creates;
            bool discards;
        };
        const Expect table[] = {
            {Gate::CompiledOut, false, false}, {Gate::TestBridge, false, false},
            {Gate::TestInstance, false, false}, {Gate::Headless, false, false},
            {Gate::Ci, false, false},          {Gate::DoNotTrack, false, true},
            {Gate::EnvOff, false, true},       {Gate::SettingOff, true, true},
            {Gate::Active, true, false},
        };
        for (const Expect& e : table) {
            QCOMPARE(createsService(e.gate), e.creates);
            QCOMPARE(discardsQueue(e.gate), e.discards);
        }
    }

    // Before QApplication: the crash handler's overload reads argv.
    void testSessionFromArgv() {
        const char* headless[] = {"gitbolt", "-platform", "offscreen"};
        QCOMPARE(gitbolt::util::detectTestSession(3, headless, envFrom({})),
                 gitbolt::util::TestSessionKind::Headless);
        const char* plain[] = {"gitbolt", "/tmp/repo"};
        QCOMPARE(gitbolt::util::detectTestSession(2, plain, envFrom({})),
                 gitbolt::util::TestSessionKind::None);
        QCOMPARE(gitbolt::util::detectTestSession(2, plain, envFrom({{"GITBOLT_TEST_BRIDGE", ""}})),
                 gitbolt::util::TestSessionKind::Bridge);

        QVERIFY(gitbolt::util::isHeadlessPlatform(u"OFFSCREEN"));
        QVERIFY(gitbolt::util::isHeadlessPlatform(u"wayland;offscreen"));
        QVERIFY(!gitbolt::util::isHeadlessPlatform(u"xcb"));
        QVERIFY(!gitbolt::util::isHeadlessPlatform(u""));
    }

    // ---- identifiers and sanitizers -----------------------------------

    void deviceIdIsDerivedAndDistinct() {
        const QString device = deviceIdFor(kInstallId);
        QVERIFY(QRegularExpression(QStringLiteral("^[0-9a-f]{32}$")).match(device).hasMatch());
        const QByteArray expected =
            QCryptographicHash::hash(("gitbolt-device-id:" + kInstallId).toUtf8(),
                                     QCryptographicHash::Sha256)
                .toHex()
                .left(32);
        QCOMPARE(device, QString::fromLatin1(expected));
        QVERIFY(device != kInstallId);
        QCOMPARE(deviceIdFor(kInstallId), device);
        QVERIFY(deviceIdFor(QStringLiteral("9c858901-8a57-4791-81fe-4c455b099bc9")) != device);
    }

    void installIdValidation() {
        QVERIFY(isValidInstallId(kInstallId));
        QVERIFY(isValidInstallId(QUuid::createUuid().toString(QUuid::WithoutBraces)));
        QVERIFY(!isValidInstallId(QString()));
        QVERIFY(!isValidInstallId(QStringLiteral("alice@example.com")));
        QVERIFY(!isValidInstallId(kInstallId.toUpper()));
        QVERIFY(!isValidInstallId(QLatin1Char('{') + kInstallId + QLatin1Char('}')));
        // Version 1, not 4.
        QVERIFY(!isValidInstallId(QStringLiteral("1b4e28ba-2fa1-11d2-883f-0016d3cca427")));
    }

    void userAgentCarriesOsTokenOnly() {
        QCOMPARE(userAgent(clientFor(QStringLiteral("macOS"), QStringLiteral("15.6"),
                                     QStringLiteral("arm64"))),
                 QStringLiteral("GitBolt/0.9.5 (Macintosh; Mac OS X 15_6)"));
        QCOMPARE(userAgent(clientFor(QStringLiteral("Windows"), QStringLiteral("11"),
                                     QStringLiteral("x86_64"))),
                 QStringLiteral("GitBolt/0.9.5 (Windows NT 10.0; Win64; x64)"));
        QCOMPARE(userAgent(clientFor(QStringLiteral("Linux"), QStringLiteral("ubuntu 24.04"),
                                     QStringLiteral("x86_64"))),
                 QStringLiteral("GitBolt/0.9.5 (X11; Linux x86_64)"));

        // This machine's: the OS token and nothing else, whatever it is.
        const ClientInfo here{QStringLiteral("1.2.3"), Environment::current()};
        const QRegularExpression shape(QStringLiteral(
            "^GitBolt/1\\.2\\.3 \\((Macintosh(; Mac OS X \\d+(_\\d+)*)?"
            "|Windows NT 10\\.0; (Win64; x64|ARM64)|X11; Linux [a-z0-9_]+)\\)$"));
        QVERIFY2(shape.match(userAgent(here)).hasMatch(), qPrintable(userAgent(here)));
    }

    void commandNames_data() {
        QTest::addColumn<QString>("objectName");
        QTest::addColumn<QString>("expected");   // empty: rejected
        QTest::newRow("explicit") << "act.commit" << "act.commit";
        QTest::newRow("menu path") << "commands.resolve-conflicts" << "commands.resolve-conflicts";
        QTest::newRow("recent entry") << "file.recent.3" << "file.recent";
        QTest::newRow("64 chars") << QString(64, u'a') << QString(64, u'a');
        QTest::newRow("65 chars") << QString(65, u'a') << QString();
        QTest::newRow("path") << "/Users/alice/secret-repo" << QString();
        QTest::newRow("branch") << "feature/secret-branch" << QString();
        QTest::newRow("space") << "a b" << QString();
        QTest::newRow("email") << "alice@example.com" << QString();
        QTest::newRow("empty") << QString() << QString();
        QTest::newRow("leading digit") << "1abc" << QString();
    }

    void commandNames() {
        QFETCH(QString, objectName);
        QFETCH(QString, expected);
        QCOMPARE(commandName(objectName).value_or(QString()), expected);
    }

    // Every literal GitService passes to operationFailed.
    void operationNames_data() {
        QTest::addColumn<QString>("literal");
        QTest::addColumn<QString>("expected");
        const QList<QPair<const char*, const char*>> table = {
            {"abort", "abort"}, {"addWorktree", "add_worktree"}, {"autostash", "autostash"},
            {"blame", "blame"},
            {"checkout", "checkout"}, {"cherryPick", "cherry_pick"},
            {"cherry-pick --abort", "cherry_pick_abort"},
            {"cherry-pick --continue", "cherry_pick_continue"},
            {"cherry-pick --skip", "cherry_pick_skip"}, {"conflicts", "conflicts"},
            {"create branch", "create_branch"}, {"createTag", "create_tag"},
            {"delete branch", "delete_branch"}, {"delete remote branch", "delete_remote_branch"},
            {"deleteTag", "delete_tag"}, {"discardFile", "discard_file"},
            {"featureFinish", "feature_finish"}, {"featureStart", "feature_start"},
            {"fetch", "fetch"}, {"fsck", "fsck"}, {"gc", "gc"}, {"gitFlowInit", "git_flow_init"},
            {"hotfixFinish", "hotfix_finish"}, {"hotfixStart", "hotfix_start"},
            {"lockWorktree", "lock_worktree"}, {"merge --abort", "merge_abort"},
            {"prune", "prune"}, {"pull", "pull"},
            {"push", "push"}, {"rebase", "rebase"}, {"rebase --abort", "rebase_abort"},
            {"rebase --continue", "rebase_continue"}, {"rebase --skip", "rebase_skip"},
            {"releaseFinish", "release_finish"}, {"releaseStart", "release_start"},
            {"removeWorktree", "remove_worktree"}, {"repack", "repack"},
            {"resolve conflicts", "resolve_conflicts"},
            {"revert --abort", "revert_abort"},
            {"revert --continue", "revert_continue"}, {"revert --skip", "revert_skip"},
            {"stage", "stage"},
            {"stage all", "stage_all"}, {"stage hunk", "stage_hunk"},
            {"stashApply", "stash_apply"}, {"stashDrop", "stash_drop"},
            {"stashPop", "stash_pop"}, {"stashSave", "stash_save"}, {"status", "status"},
            {"submoduleInit", "submodule_init"}, {"submoduleUpdate", "submodule_update"},
            {"submodules", "submodules"},
            {"unlockWorktree", "unlock_worktree"}, {"unstage", "unstage"},
            {"unstageAll", "unstage_all"},
            // Not literals GitService sends: never passed through.
            {"push origin feature/secret-branch", "other"}, {"", "other"},
            {"fatal: repository '/home/bob/x' not found", "other"},
        };
        for (const auto& [literal, expected] : table)
            QTest::newRow(literal) << QString::fromLatin1(literal) << QString::fromLatin1(expected);
    }

    void operationNames() {
        QFETCH(QString, literal);
        QFETCH(QString, expected);
        QCOMPARE(operationName(literal), expected);
    }

    // openErrorName() mirrors git::GitErrorCode by value without
    // linking git; this pins the two together.
    void openErrorNamesPinned() {
        using gitbolt::git::GitErrorCode;
        const QList<QPair<GitErrorCode, const char*>> table = {
            {GitErrorCode::GenericError, "generic_error"}, {GitErrorCode::NotFound, "not_found"},
            {GitErrorCode::Exists, "exists"}, {GitErrorCode::Ambiguous, "ambiguous"},
            {GitErrorCode::BufferTooShort, "buffer_too_short"}, {GitErrorCode::User, "user"},
            {GitErrorCode::BareRepo, "bare_repo"}, {GitErrorCode::UnbornBranch, "unborn_branch"},
            {GitErrorCode::Unmerged, "unmerged"},
            {GitErrorCode::NonFastForward, "non_fast_forward"},
            {GitErrorCode::InvalidSpec, "invalid_spec"}, {GitErrorCode::Conflict, "conflict"},
            {GitErrorCode::Locked, "locked"}, {GitErrorCode::Modified, "modified"},
            {GitErrorCode::Auth, "auth"}, {GitErrorCode::Certificate, "certificate"},
            {GitErrorCode::Applied, "applied"}, {GitErrorCode::Peel, "peel"},
            {GitErrorCode::Eof, "eof"}, {GitErrorCode::Invalid, "invalid"},
            {GitErrorCode::Uncommitted, "uncommitted"}, {GitErrorCode::Directory, "directory"},
            {GitErrorCode::MergeConflict, "merge_conflict"},
            {GitErrorCode::ProcessFailed, "process_failed"},
            {GitErrorCode::CheckoutFailed, "checkout_failed"},
        };
        for (const auto& [code, name] : table)
            QCOMPARE(openErrorName(int(code)), QString::fromLatin1(name));
        QCOMPARE(openErrorName(int(GitErrorCode::Ok)), QStringLiteral("other"));
        QCOMPARE(openErrorName(int(GitErrorCode::CheckoutFailed) + 1), QStringLiteral("other"));
        QCOMPARE(openErrorName(-1), QStringLiteral("other"));
    }

    void repoOpenedBuckets_data() {
        QTest::addColumn<int>("commits");
        QTest::addColumn<QString>("bucket");
        QTest::newRow("0") << 0 << "0";
        QTest::newRow("1") << 1 << "1-99";
        QTest::newRow("99") << 99 << "1-99";
        QTest::newRow("100") << 100 << "100-255";
        QTest::newRow("255") << 255 << "100-255";
        QTest::newRow("256") << 256 << "256+";
        QTest::newRow("negative") << -5 << "0";
    }

    void repoOpenedBuckets() {
        QFETCH(int, commits);
        QFETCH(QString, bucket);
        QCOMPARE(commitsBucket(commits), bucket);
        QCOMPARE(clampDurationMs(-1), qint64(0));
        QCOMPARE(clampDurationMs(412), qint64(412));
        QCOMPARE(clampDurationMs(10'000'000), qint64(600000));
    }

    void refusesPlainHttpOffLoopback() {
        auto config = [](const QString& host) {
            return Config::fromEndpoints(host, QStringLiteral("id"), QStringLiteral("secret"),
                                         kGlitchTipKey, QStringLiteral("7"),
                                         QStringLiteral("0.9.5"), QStringLiteral("/tmp/q"));
        };
        QVERIFY(config(QStringLiteral("https://telemetry.example.com")).isComplete());
        QVERIFY(config(QStringLiteral("http://127.0.0.1:8080")).isComplete());
        QVERIFY(config(QStringLiteral("http://[::1]:8080")).isComplete());
        QVERIFY(!config(QStringLiteral("http://example.com")).isComplete());
        QVERIFY(!config(QStringLiteral("http://localhost:8080")).isComplete());
        QVERIFY(!config(QStringLiteral("http://10.0.0.1")).isComplete());
        QVERIFY(!config(QStringLiteral("ftp://example.com")).isComplete());
        QVERIFY(!config(QStringLiteral("https://user@example.com")).isComplete());
        QVERIFY(!config(QString()).isComplete());

        Config missing = config(QStringLiteral("https://example.com"));
        missing.openPanelSecret.clear();
        QVERIFY(!missing.isComplete());
        Config badProject = Config::fromEndpoints(
            QStringLiteral("https://example.com"), QStringLiteral("id"), QStringLiteral("s"),
            kGlitchTipKey, QStringLiteral("abc"), QStringLiteral("0.9.5"), QStringLiteral("/q"));
        QVERIFY(!badProject.isComplete());
        Config badVersion = config(QStringLiteral("https://example.com"));
        badVersion.appVersion = QStringLiteral("unknown");
        QVERIFY(!badVersion.isComplete());
        QVERIFY(!Config().isComplete());
    }

    void classifyOutcomes_data() {
        QTest::addColumn<int>("status");
        QTest::addColumn<QNetworkReply::NetworkError>("error");
        QTest::addColumn<Outcome>("expected");
        QTest::addColumn<bool>("requestSent");
        using E = QNetworkReply::NetworkError;
        // Nothing of the request sent: Qt's catch-all is how a connect
        // that found no route ("Host unreachable", "Network
        // unreachable") comes; a TLS failure or a timeout still counts.
        QTest::newRow("no route") << 0 << E::UnknownNetworkError << Outcome::Unreachable << false;
        QTest::newRow("tls, nothing sent") << 0 << E::SslHandshakeFailedError << Outcome::Retry
                                           << false;
        QTest::newRow("timeout, nothing sent") << 0 << E::TimeoutError << Outcome::Retry << false;
        QTest::newRow("closed, nothing sent") << 0 << E::RemoteHostClosedError << Outcome::Retry
                                              << false;
        QTest::newRow("refused, nothing sent") << 0 << E::ConnectionRefusedError
                                               << Outcome::Unreachable << false;
        QTest::newRow("200") << 200 << E::NoError << Outcome::Sent << true;
        QTest::newRow("202") << 202 << E::NoError << Outcome::Sent << true;
        QTest::newRow("301") << 301 << E::NoError << Outcome::Drop << true;
        QTest::newRow("302") << 302 << E::NoError << Outcome::Drop << true;
        QTest::newRow("400") << 400 << E::ProtocolInvalidOperationError << Outcome::Drop << true;
        QTest::newRow("401") << 401 << E::AuthenticationRequiredError << Outcome::Drop << true;
        QTest::newRow("404") << 404 << E::ContentNotFoundError << Outcome::Drop << true;
        QTest::newRow("408") << 408 << E::UnknownContentError << Outcome::Retry << true;
        QTest::newRow("413") << 413 << E::UnknownContentError << Outcome::Drop << true;
        QTest::newRow("429") << 429 << E::UnknownContentError << Outcome::Retry << true;
        QTest::newRow("500") << 500 << E::InternalServerError << Outcome::Retry << true;
        QTest::newRow("503") << 503 << E::ServiceUnavailableError << Outcome::Retry << true;
        QTest::newRow("transfer timeout") << 0 << E::TimeoutError << Outcome::Retry << true;
        QTest::newRow("cancel not ours") << 0 << E::OperationCanceledError << Outcome::Retry << true;
        QTest::newRow("tls") << 0 << E::SslHandshakeFailedError << Outcome::Retry << true;
        QTest::newRow("closed") << 0 << E::RemoteHostClosedError << Outcome::Retry << true;
        QTest::newRow("unknown network error") << 0 << E::UnknownNetworkError << Outcome::Retry << true;
        // Never reached a server: offline, DNS down, nothing listening.
        QTest::newRow("refused") << 0 << E::ConnectionRefusedError << Outcome::Unreachable << true;
        QTest::newRow("dns") << 0 << E::HostNotFoundError << Outcome::Unreachable << true;
        QTest::newRow("network down") << 0 << E::TemporaryNetworkFailureError
                                      << Outcome::Unreachable << true;
        QTest::newRow("network session") << 0 << E::NetworkSessionFailedError
                                         << Outcome::Unreachable << true;
        QTest::newRow("proxy not found") << 0 << E::ProxyNotFoundError << Outcome::Unreachable << true;
        QTest::newRow("proxy refused") << 0 << E::ProxyConnectionRefusedError
                                       << Outcome::Unreachable << true;
        // A status always means a server answered.
        QTest::newRow("503 with a refused error") << 503 << E::ConnectionRefusedError
                                                  << Outcome::Retry << true;
    }

    void classifyOutcomes() {
        QFETCH(int, status);
        QFETCH(QNetworkReply::NetworkError, error);
        QFETCH(Outcome, expected);
        QFETCH(bool, requestSent);
        QCOMPARE(classify(status, error, requestSent), expected);
    }

    // ---- what goes on the wire ----------------------------------------

    void launchEventShape() {
        Provider provider;
        const Config config = testConfig(*stub_, dir_->path());
        auto service = makeService(config, provider);
        QVERIFY(service->isActive());
        service->recordLaunch();
        service->start();
        QTRY_COMPARE(stub_->requests().size(), 1);

        const StubRequest& request = stub_->requests().first();
        QCOMPARE(request.method, QByteArray("POST"));
        QCOMPARE(request.path, QByteArray("/track"));
        QCOMPARE(request.headerNames(), gitbolt::test::kOpenPanelHeaders);
        QCOMPARE(request.header("openpanel-client-id"), QByteArray("test-client"));
        QCOMPARE(request.header("openpanel-client-secret"), QByteArray("test-key-value"));
        QCOMPARE(request.header("content-type"), QByteArray("application/json"));
        QCOMPARE(request.header("accept-language"), QByteArray("*"));
        QCOMPARE(request.header("accept-encoding"), QByteArray("identity"));
        const QString agent = QString::fromLatin1(request.header("user-agent"));
        QVERIFY2(QRegularExpression(QStringLiteral("^GitBolt/1\\.2\\.3 \\((Macintosh|Windows NT|X11)"
                                                   "[^)]*\\)$"))
                     .match(agent)
                     .hasMatch(),
                 qPrintable(agent));

        const QJsonObject body = request.json();
        QCOMPARE(keySet(body), (QSet<QString>{QStringLiteral("type"), QStringLiteral("payload")}));
        QCOMPARE(body.value(QStringLiteral("type")).toString(), QStringLiteral("track"));
        const QJsonObject payload = body.value(QStringLiteral("payload")).toObject();
        QCOMPARE(keySet(payload), (QSet<QString>{QStringLiteral("name"), QStringLiteral("profileId"),
                                                 QStringLiteral("properties")}));
        QCOMPARE(payload.value(QStringLiteral("name")).toString(), QStringLiteral("app_launched"));
        QCOMPARE(payload.value(QStringLiteral("profileId")).toString(), kInstallId);

        // Exactly the allowlist; a fresh event has no __timestamp (the
        // server's receipt time stands), and there is never an __ip.
        const QJsonObject props = properties(request);
        QCOMPARE(keySet(props),
                 (QSet<QString>{QStringLiteral("app_version"), QStringLiteral("__deviceId"),
                                QStringLiteral("__os"), QStringLiteral("__osVersion"),
                                QStringLiteral("__device"), QStringLiteral("arch"),
                                QStringLiteral("qt_version"), QStringLiteral("locale")}));
        QCOMPARE(props.value(QStringLiteral("__deviceId")).toString(), deviceIdFor(kInstallId));
        QVERIFY(props.value(QStringLiteral("__deviceId")).toString() != kInstallId);
        QVERIFY((QStringList{QStringLiteral("macOS"), QStringLiteral("Windows"),
                             QStringLiteral("Linux")})
                    .contains(props.value(QStringLiteral("__os")).toString()));
        QCOMPARE(props.value(QStringLiteral("__device")).toString(), QStringLiteral("desktop"));
        QCOMPARE(props.value(QStringLiteral("app_version")).toString(), QStringLiteral("1.2.3"));
        QVERIFY(QRegularExpression(QStringLiteral("^[a-z]{2,3}$|^und$"))
                    .match(props.value(QStringLiteral("locale")).toString())
                    .hasMatch());
        QVERIFY(!request.body.contains("__ip"));
        QVERIFY(!request.body.contains("__timestamp"));
        QCOMPARE(provider.calls, 1);
    }

    // A record that waited (here: written by an earlier session five
    // minutes ago) says when it happened; the age is this client's
    // clock on both ends.
    void reloadedOldRecordCarriesTimestamp() {
        Provider provider;
        const Config config = testConfig(*stub_, dir_->path());
        const QDateTime queued = QDateTime::currentDateTimeUtc().addSecs(-300);
        {
            EventQueue queue(config.queueDir, {});
            QVERIFY(queue.open());
            QueuedRecord record;
            record.queued = queued;
            record.payload = *appLaunchedEvent(
                ClientInfo{QStringLiteral("1.2.3"), Environment::current()}, kInstallId);
            queue.append(record);
        }
        auto service = makeService(config, provider);
        service->start();
        QTRY_COMPARE(stub_->requests().size(), 1);
        QCOMPARE(properties(stub_->requests().first()).value(QStringLiteral("__timestamp")).toString(),
                 queued.toString(Qt::ISODateWithMs));
        // Nothing was built this session: the ID wasn't needed.
        QCOMPARE(provider.calls, 0);
    }

    void oneEventPerRequestAndPacing() {
        Provider provider;
        auto service = makeService(testConfig(*stub_, dir_->path()), provider);
        service->recordRepoOpened(10, 20, 1);
        service->recordRepoOpened(11, 21, 100);
        service->recordRepoOpened(12, 22, 300);
        service->start();
        QTRY_COMPARE(stub_->requests().size(), 3);

        const QList<StubRequest>& requests = stub_->requests();
        for (qsizetype i = 0; i < requests.size(); ++i) {
            const QJsonObject payload = requests.at(i).json().value(QStringLiteral("payload")).toObject();
            QCOMPARE(payload.value(QStringLiteral("name")).toString(), QStringLiteral("repo_opened"));
            QCOMPARE(properties(requests.at(i)).value(QStringLiteral("open_ms")).toInt(), 10 + int(i));
            if (i == 0)
                continue;
            // One at a time, paced: each arrives after the previous was
            // answered, and at least ~pace later.
            QVERIFY(requests.at(i).arrivedMs >= requests.at(i - 1).answeredMs);
            QVERIFY2(requests.at(i).arrivedMs - requests.at(i - 1).arrivedMs >= 40,
                     qPrintable(QString::number(requests.at(i).arrivedMs
                                                - requests.at(i - 1).arrivedMs)));
        }
        const QStringList buckets = {
            properties(requests.at(0)).value(QStringLiteral("commits_bucket")).toString(),
            properties(requests.at(1)).value(QStringLiteral("commits_bucket")).toString(),
            properties(requests.at(2)).value(QStringLiteral("commits_bucket")).toString()};
        QCOMPARE(buckets, (QStringList{QStringLiteral("1-99"), QStringLiteral("100-255"),
                                       QStringLiteral("256+")}));
        QTRY_COMPARE(service->queuedCount(), 0);
    }

    void flushBudget() {
        Provider provider;
        const Config config = testConfig(*stub_, dir_->path());
        auto service = makeService(config, provider);
        for (int i = 0; i < config.maxPerFlush + 2; ++i)
            service->recordRepoOpened(i, i, 1);
        service->start();
        QTRY_COMPARE(stub_->requests().size(), config.maxPerFlush);
        QTest::qWait(300);
        QCOMPARE(stub_->requests().size(), config.maxPerFlush);
        // The rest wait for the next tick, on disk (the cycle's rewrite).
        QCOMPARE(queueLines(config).size(), 2);
        QCOMPARE(service->queuedCount(), 2);
    }

    // Records go out ~firstFlushDelayMs after the first unsent one, not
    // at the 15-minute tick; a later record doesn't postpone that. The
    // delay leaves a loaded CI runner over a second of slack on the
    // two waits below.
    void debouncedFlushAfterAppend() {
        Provider provider;
        Config config = testConfig(*stub_, dir_->path());
        config.firstFlushDelayMs = 1500;
        auto service = makeService(config, provider);
        service->start();
        QElapsedTimer clock;
        clock.start();
        service->recordRepoOpened(1, 1, 1);
        QTest::qWait(150);
        service->recordRepoOpened(2, 2, 1);
        QTest::qWait(150);
        QCOMPARE(stub_->requests().size(), 0);
        QTRY_COMPARE(stub_->requests().size(), 2);
        // Well before the retry tick (60 s here), with room for a slow CI box.
        QVERIFY2(clock.elapsed() < 5000, qPrintable(QString::number(clock.elapsed())));
    }

    // After a failure, new records wait for the retry tick too: an
    // outage costs one request per tick, however busy the user is.
    // Checked against the tick, which start() sets going, rather than
    // a fixed wait after the first request: on a slow box that one can
    // come so late (the first request sets up the network stack) that
    // the tick falls inside the wait, and the retry is then on time.
    void backoffHoldsNewRecordsUntilTick() {
        Provider provider;
        Config config = testConfig(*stub_, dir_->path());
        config.flushIntervalMs = 700;
        int answered = 0;
        stub_->respond = [&answered](const StubRequest&) {
            return StubResponse{answered++ == 0 ? 503 : 202, {}};
        };
        auto service = makeService(config, provider);
        service->recordRepoOpened(1, 1, 1);
        const qint64 tick = stub_->elapsedMs() + config.flushIntervalMs;
        service->start();
        // Not size() == 1: on a slow box the first request can come so
        // close to the tick that the retry follows before a poll sees
        // one. A new record then goes after the tick all the same.
        QTRY_VERIFY(!stub_->requests().isEmpty());
        service->recordRepoOpened(2, 2, 1);
        QTRY_COMPARE(stub_->requests().size(), 3);
        // Neither the retry nor the new record went before the tick,
        // less 10%: a coarse timer may fire early by 5% of its interval
        // (sending early by a whole interval is what this catches).
        for (int i = 1; i < 3; ++i)
            QVERIFY2(stub_->requests().at(i).arrivedMs >= tick - config.flushIntervalMs / 10,
                     qPrintable(QStringLiteral("request %1 at %2 ms, tick at %3 ms")
                                    .arg(i).arg(stub_->requests().at(i).arrivedMs).arg(tick)));
        // The retried record went first, then the new one.
        QCOMPARE(properties(stub_->requests().at(1)).value(QStringLiteral("open_ms")).toInt(), 1);
        QCOMPARE(properties(stub_->requests().at(2)).value(QStringLiteral("open_ms")).toInt(), 2);
    }

    // A flush armed before a request failed must not send before the
    // next retry tick: here the tick fails first (600 ms), the armed
    // flush comes due inside the back-off (900 ms), and only the next
    // tick (1200 ms) may try again.
    void armedFlushRespectsBackoff() {
        Provider provider;
        Config config = testConfig(*stub_, dir_->path());
        config.flushIntervalMs = 600;
        config.firstFlushDelayMs = 900;
        int answered = 0;
        stub_->respond = [&answered](const StubRequest&) {
            return StubResponse{answered++ == 0 ? 503 : 202, {}};
        };
        QElapsedTimer clock;
        clock.start();
        auto service = makeService(config, provider);
        service->recordRepoOpened(1, 1, 1);
        service->start();
        QTRY_COMPARE(stub_->requests().size(), 1);
        while (clock.elapsed() < 1050)
            QTest::qWait(10);
        QCOMPARE(stub_->requests().size(), 1);
        QTRY_COMPARE(stub_->requests().size(), 2);
        QVERIFY2(clock.elapsed() >= 1100, qPrintable(QString::number(clock.elapsed())));
    }

    void transientFailureEndsCycle_data() {
        QTest::addColumn<int>("status");    // -1: never answer
        QTest::newRow("503") << 503;
        QTest::newRow("429") << 429;
        QTest::newRow("408") << 408;
        QTest::newRow("no answer (transfer timeout)") << -1;
    }

    void transientFailureEndsCycle() {
        QFETCH(int, status);
        Provider provider;
        Config config = testConfig(*stub_, dir_->path());
        config.flushIntervalMs = 500;
        // The queue file as each request arrives: the previous cycle's
        // rewrite is on disk by then, and nothing changes it until this
        // request's answer.
        QList<QList<QJsonObject>> atArrival;
        stub_->respond = [&, status](const StubRequest&) {
            atArrival << queueLines(config);
            return StubResponse{status, {}};
        };

        auto service = makeService(config, provider);
        service->recordRepoOpened(1, 1, 1);
        service->recordRepoOpened(2, 2, 1);
        const qint64 started = stub_->elapsedMs();
        service->start();

        // First cycle: one request, then the cycle ends with the front
        // record's attempt counted on disk, as the retry finds it. Read
        // when the retry arrives, not polled: that state lasts only
        // until the retry fails, which a poll can miss on a slow box.
        // (That the second record isn't tried in the first cycle, the
        // open_ms check at the end covers.)
        QTRY_VERIFY(atArrival.size() >= 2);
        QCOMPARE(atArrival.at(1).size(), 2);
        QCOMPARE(atArrival.at(1).at(0).value(QStringLiteral("attempts")).toInt(), 1);
        QCOMPARE(atArrival.at(1).at(1).value(QStringLiteral("attempts")).toInt(), 0);

        // Ticks retry it until maxAttempts, then it is dropped; one
        // request per tick, never two in one cycle.
        QTRY_VERIFY_WITH_TIMEOUT(
            !queueLines(config).isEmpty()
                && queueLines(config).first().value(QStringLiteral("payload")).toObject()
                           .value(QStringLiteral("payload")).toObject()
                           .value(QStringLiteral("properties")).toObject()
                           .value(QStringLiteral("open_ms")).toInt() == 2,
            10000);
        QVERIFY(stub_->requests().size() >= config.maxAttempts);
        const QList<StubRequest>& requests = stub_->requests();
        // The i-th retry is not before the i-th tick, less 10% a tick: a
        // coarse timer may fire early by 5% of its interval, and that
        // adds up from tick to tick (two in one cycle would be a whole
        // interval early). The ticks run from start(), the first request
        // from the first flush, so the gap between those two can be
        // anything.
        for (int i = 1; i < config.maxAttempts; ++i)
            QVERIFY2(requests.at(i).arrivedMs - started
                         >= i * (config.flushIntervalMs - config.flushIntervalMs / 10),
                     qPrintable(QStringLiteral("retry %1 at %2 ms after start")
                                    .arg(i).arg(requests.at(i).arrivedMs - started)));
        for (int i = 0; i < config.maxAttempts; ++i)
            QCOMPARE(properties(requests.at(i)).value(QStringLiteral("open_ms")).toInt(), 1);
    }

    // A request no server saw (nothing listening here; offline or DNS
    // down for real) ends the cycle like a Retry, but counts no attempt
    // and writes nothing: attempts are for a server that keeps refusing
    // a record, and time offline used to drop one every few ticks.
    void unreachableCostsNoAttempt_data() {
        QTest::addColumn<bool>("noRoute");
        QTest::newRow("connection refused") << false;
        // 0.0.0.1: no route, so the connect fails at once ("Host
        // unreachable"), and Qt says UnknownNetworkError. Such tries
        // used to count, offline with a cached DNS answer.
        QTest::newRow("no route") << true;
    }

    void unreachableCostsNoAttempt() {
        QFETCH(bool, noRoute);
        Provider provider;
        Config config = testConfig(*stub_, dir_->path());
        config.flushIntervalMs = 300;
        // So the refusal, not the transfer timeout (which counts), ends
        // each request: Windows takes about 2 s to refuse a connection.
        config.transferTimeoutMs = 10000;
        const int port = closedPort();
        config.trackUrl.setPort(port);
        config.storeUrl.setPort(port);
        if (noRoute) {
            // https: plain http goes to loopback only (Config). The
            // connect fails before any TLS.
            for (QUrl* url : {&config.trackUrl, &config.storeUrl}) {
                url->setScheme(QStringLiteral("https"));
                url->setHost(QStringLiteral("0.0.0.1"));
                url->setPort(9);
            }
            // Where the platform reports it otherwise (a route after
            // all, which times out), this row has nothing to show.
            QNetworkAccessManager probe;
            QNetworkRequest request(config.trackUrl);
            request.setTransferTimeout(3000);
            request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("text/plain"));
            QNetworkReply* reply = probe.post(request, QByteArrayLiteral("{}"));
            QTRY_VERIFY_WITH_TIMEOUT(reply->isFinished(), 10000);
            const auto error = reply->error();
            delete reply;
            if (error != QNetworkReply::UnknownNetworkError)
                QSKIP(qPrintable(QStringLiteral("0.0.0.1 gives network error %1 here, not "
                                                "UnknownNetworkError").arg(error)));
        }

        auto service = makeService(config, provider);
        service->recordRepoOpened(1, 1, 1);
        service->recordRepoOpened(2, 2, 1);
        const quint64 writes = EventQueue::diskWrites();
        service->start();

        // The service's manager, created at the first flush, sees every
        // request finish. More of them than maxAttempts: each one used
        // to count an attempt, and the front record was dropped.
        QTRY_VERIFY(service->findChild<QNetworkAccessManager*>());
        QSignalSpy finished(service->findChild<QNetworkAccessManager*>(),
                            &QNetworkAccessManager::finished);
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() > config.maxAttempts, 20000);

        QCOMPARE(service->queuedCount(), 2);
        const QList<QJsonObject> lines = queueLines(config);
        QCOMPARE(lines.size(), 2);
        for (const QJsonObject& line : lines)
            QCOMPARE(line.value(QStringLiteral("attempts")).toInt(), 0);
        QCOMPARE(EventQueue::diskWrites(), writes);
        QCOMPARE(stub_->requests().size(), 0);
    }

    void permanentFailureDrops_data() {
        QTest::addColumn<int>("status");
        QTest::newRow("400") << 400;
        QTest::newRow("401") << 401;
        QTest::newRow("413") << 413;
        QTest::newRow("302 with Set-Cookie") << 302;
    }

    // Dropped at once, and the next record goes in the same cycle:
    // without a cookie from the redirect, and the redirect is never
    // followed (the stub flags any request to another path).
    void permanentFailureDrops() {
        QFETCH(int, status);
        Provider provider;
        const Config config = testConfig(*stub_, dir_->path());
        const QByteArray elsewhere = stub_->origin().toLatin1() + "/elsewhere";
        int answered = 0;
        stub_->respond = [&answered, status, elsewhere](const StubRequest&) {
            if (answered++ > 0)
                return StubResponse{202, {}};
            StubResponse response{status, {}};
            if (status == 302)
                response.headers = {{"Location", elsewhere}, {"Set-Cookie", "tracker=1; Path=/"}};
            return response;
        };
        auto service = makeService(config, provider);
        service->recordRepoOpened(1, 1, 1);
        service->recordRepoOpened(2, 2, 1);
        service->start();
        QTRY_COMPARE(stub_->requests().size(), 2);
        QTRY_COMPARE(service->queuedCount(), 0);
        QTest::qWait(200);
        QCOMPARE(stub_->requests().size(), 2);
        QCOMPARE(stub_->requests().at(1).header("cookie"), QByteArray());
        QCOMPARE(properties(stub_->requests().at(1)).value(QStringLiteral("open_ms")).toInt(), 2);
        // The same cycle, not the next tick (60 s here).
        QVERIFY(stub_->requests().at(1).arrivedMs - stub_->requests().at(0).arrivedMs < 5000);
    }

    // ---- the off switch, quit, restarts --------------------------------

    void disableStopsAndDiscards() {
        Provider provider;
        Config config = testConfig(*stub_, dir_->path());
        config.transferTimeoutMs = 30000;
        config.flushIntervalMs = 100;
        stub_->respond = [](const StubRequest&) { return StubResponse{-1, {}}; };
        auto service = makeService(config, provider);
        service->recordRepoOpened(1, 1, 1);
        service->start();
        QTRY_COMPARE(stub_->requests().size(), 1);
        QVERIFY(QFile::exists(EventQueue::filePath(config.queueDir)));

        service->setUserEnabled(false);
        QVERIFY(!service->isActive());
        QVERIFY(!QFile::exists(EventQueue::filePath(config.queueDir)));
        QVERIFY(!QDir(config.queueDir).exists());
        QTRY_VERIFY(stub_->disconnects() >= 1);

        // Ticks fire, records are refused, quit writes nothing.
        const quint64 writes = EventQueue::diskWrites();
        QTest::qWait(350);
        service->recordLaunch();
        service->countCommand(QStringLiteral("act.commit"));
        service->recordError(ErrorKind::CommitFailed);
        service->persistPending();
        QCOMPARE(stub_->requests().size(), 1);
        QCOMPARE(EventQueue::diskWrites(), writes);
        QVERIFY(!QDir(config.queueDir).exists());

        // And back on.
        stub_->respond = [](const StubRequest&) { return StubResponse{}; };
        service->setUserEnabled(true);
        QVERIFY(service->isActive());
        service->recordRepoOpened(2, 2, 1);
        QTRY_COMPARE(stub_->requests().size(), 2);
        QCOMPARE(properties(stub_->requests().at(1)).value(QStringLiteral("open_ms")).toInt(), 2);
    }

    // abort() reports finished() synchronously; a handler that ran
    // would count the abort as an attempt and rewrite the queue right
    // before the discard.
    void disableDuringRequestWritesNothingBeforeDiscard() {
        Provider provider;
        Config config = testConfig(*stub_, dir_->path());
        config.transferTimeoutMs = 30000;
        stub_->respond = [](const StubRequest&) { return StubResponse{-1, {}}; };
        auto service = makeService(config, provider);
        service->recordRepoOpened(1, 1, 1);
        service->countCommand(QStringLiteral("act.commit"));
        service->start();
        QTRY_COMPARE(stub_->requests().size(), 1);

        const quint64 writes = EventQueue::diskWrites();
        service->setUserEnabled(false);
        QCOMPARE(EventQueue::diskWrites(), writes);
        QVERIFY(!QFile::exists(EventQueue::filePath(config.queueDir)));
    }

    // The switch turned off in another process (a second GitBolt that
    // couldn't take the instance lock, a test session sharing the INI):
    // no settingsChanged reaches this one, and the other process can't
    // delete the queue this one holds. The service re-reads the switch
    // at every flush cycle, before each request of one, and at quit,
    // and stops as if it had been turned off here: nothing more sent,
    // the queue deleted.
    void switchOffElsewhereStopsHolder_data() {
        QTest::addColumn<QString>("when");
        QTest::newRow("before the first flush") << QStringLiteral("flush");
        QTest::newRow("during the backoff") << QStringLiteral("backoff");
        QTest::newRow("between two requests of a flush") << QStringLiteral("cycle");
        QTest::newRow("at quit") << QStringLiteral("quit");
    }

    void switchOffElsewhereStopsHolder() {
        QFETCH(QString, when);
        Provider provider;
        Config config = testConfig(*stub_, dir_->path());
        config.firstFlushDelayMs = when == QLatin1String("quit") ? 60000 : 200;
        config.flushIntervalMs = 300;
        bool persisted = true;
        stub_->respond = [](const StubRequest&) { return StubResponse{503, {}}; };
        qsizetype sent = 0;
        if (when == QLatin1String("backoff")) {
            // The first request fails, and the switch goes off elsewhere
            // while it is answered: the next tick would retry it. Turned
            // off once the failure showed on disk, it came too late now
            // and then (CI, 1 run in 40 here): with 300 ms between ticks,
            // the retry had gone out by the time the poll saw the first
            // failure, so two requests were made before the switch was
            // off, as the service rightly reported.
            stub_->respond = [&persisted](const StubRequest&) {
                persisted = false;
                return StubResponse{503, {}};
            };
            sent = 1;
        } else if (when == QLatin1String("cycle")) {
            // A flush of five, paced: the switch goes off elsewhere as
            // the second is answered. The switch was read at the start
            // of the cycle alone, and the other three went out all the
            // same (with the shipped pace and budget, up to a minute
            // of requests after the switch was off).
            config.paceMs = 150;
            stub_->respond = [&persisted, answered = 0](const StubRequest&) mutable {
                if (++answered == 2)
                    persisted = false;
                return StubResponse{};
            };
            sent = 2;
        }
        TelemetryService service(config, provider.fn(), true, [&persisted] { return persisted; });
        service.start();
        service.recordLaunch();
        if (when == QLatin1String("cycle")) {
            for (int i = 1; i <= 4; ++i)
                service.recordRepoOpened(i, i, 1);
            QCOMPARE(service.queuedCount(), 5);
        }
        if (when == QLatin1String("backoff") || when == QLatin1String("cycle"))
            QTRY_COMPARE(stub_->requests().size(), sent);

        persisted = false;
        if (when == QLatin1String("quit"))
            service.persistPending();
        else
            QTest::qWait(700);   // past the first flush, and two ticks
        QCOMPARE(stub_->requests().size(), sent);
        QVERIFY(!service.isActive());
        QVERIFY(!QFile::exists(EventQueue::filePath(config.queueDir)));
        QVERIFY(!QDir(config.queueDir).exists());
    }

    void persistPendingDuringRequestKeepsAttempts() {
        Provider provider;
        Config config = testConfig(*stub_, dir_->path());
        config.transferTimeoutMs = 30000;
        stub_->respond = [](const StubRequest&) { return StubResponse{-1, {}}; };
        auto service = makeService(config, provider);
        service->recordRepoOpened(1, 1, 1);
        service->countCommand(QStringLiteral("act.commit"));
        service->start();
        QTRY_COMPARE(stub_->requests().size(), 1);

        service->persistPending();
        QVERIFY(!service->isActive());
        const QList<QJsonObject> lines = queueLines(config);
        QCOMPARE(lines.size(), 2);   // the in-flight record, then the counts
        QCOMPARE(lines.at(0).value(QStringLiteral("attempts")).toInt(), 0);
        QTRY_VERIFY(stub_->disconnects() >= 1);
        QTest::qWait(100);
        QCOMPARE(stub_->requests().size(), 1);
    }

    void quitDuringRequestSixTimesKeepsRecord() {
        Provider provider;
        Config config = testConfig(*stub_, dir_->path());
        config.transferTimeoutMs = 30000;
        stub_->respond = [](const StubRequest&) { return StubResponse{-1, {}}; };
        auto service = makeService(config, provider);
        service->recordRepoOpened(7, 7, 1);
        for (int run = 1; run <= 6; ++run) {
            if (!service)
                service = makeService(config, provider);
            service->start();
            QTRY_COMPARE(stub_->requests().size(), run);
            service->persistPending();
            service.reset();
            const QList<QJsonObject> lines = queueLines(config);
            QCOMPARE(lines.size(), 1);
            QCOMPARE(lines.first().value(QStringLiteral("attempts")).toInt(), 0);
        }

        stub_->respond = [](const StubRequest&) { return StubResponse{}; };
        service = makeService(config, provider);
        service->start();
        QTRY_COMPARE(stub_->requests().size(), 7);
        QTRY_COMPARE(service->queuedCount(), 0);
        QCOMPARE(properties(stub_->requests().last()).value(QStringLiteral("open_ms")).toInt(), 7);
    }

    void persistsAcrossRestart() {
        Provider provider;
        const Config config = testConfig(*stub_, dir_->path());
        stub_->respond = [](const StubRequest&) { return StubResponse{503, {}}; };
        auto service = makeService(config, provider);
        service->recordLaunch();
        service->start();
        // Wait for the 503 to be handled (the cycle ends with the attempt
        // counted on disk), not just for the stub to see the request:
        // quitting while the reply is still on its way aborts it, and an
        // abort is rightly not counted. Slower runners hit that window.
        QTRY_VERIFY(!queueLines(config).isEmpty()
                    && queueLines(config).first().value(QStringLiteral("attempts")).toInt() == 1);
        QCOMPARE(stub_->requests().size(), 1);
        service->persistPending();
        service.reset();
        QCOMPARE(queueLines(config).size(), 1);
        QCOMPARE(queueLines(config).first().value(QStringLiteral("attempts")).toInt(), 1);

        stub_->respond = [](const StubRequest&) { return StubResponse{}; };
        service = makeService(config, provider);
        service->start();
        QTRY_COMPARE(stub_->requests().size(), 2);
        // The same event, as built when it was queued (younger than
        // timestampAfterMs, so still without __timestamp).
        QCOMPARE(stub_->requests().at(1).json(), stub_->requests().at(0).json());
        QTRY_COMPARE(service->queuedCount(), 0);
        service->persistPending();
        QVERIFY(!QFile::exists(EventQueue::filePath(config.queueDir)));
    }

    // ---- the queue ----------------------------------------------------

    void queueCaps() {
        const QString dir = dir_->path() + QStringLiteral("/q");
        EventQueue queue(dir, {10, 1024 * 1024, 14, 3});
        QVERIFY(queue.open());
        for (int i = 0; i < 110; ++i) {
            QueuedRecord record;
            record.queued = QDateTime::currentDateTimeUtc();
            record.payload = QJsonObject{{QStringLiteral("n"), i}};
            queue.append(record);
        }
        QCOMPARE(queue.size(), 10);
        QCOMPARE(queue.front().payload.value(QStringLiteral("n")).toInt(), 100);
        QVERIFY(queue.sync());
        QFile file(EventQueue::filePath(dir));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll().count('\n'), 10);
    }

    // Over the caps, analytics go first, then errors, the crash last;
    // and there is only ever one crash.
    void queueDropOrder() {
        const QString dir = dir_->path() + QStringLiteral("/q");
        EventQueue queue(dir, {3, 1024 * 1024, 14, 3});
        QVERIFY(queue.open());
        auto record = [](Sink sink, const char* level, int n) {
            QueuedRecord r;
            r.sink = sink;
            r.queued = QDateTime::currentDateTimeUtc();
            r.payload = QJsonObject{{QStringLiteral("level"), QLatin1String(level)},
                                    {QStringLiteral("n"), n}};
            return r;
        };
        queue.append(record(Sink::GlitchTip, "fatal", 1));
        queue.append(record(Sink::GlitchTip, "fatal", 2));
        QCOMPARE(queue.size(), 1);
        QCOMPARE(queue.front().payload.value(QStringLiteral("n")).toInt(), 2);

        queue.append(record(Sink::GlitchTip, "error", 3));
        queue.append(record(Sink::OpenPanel, "", 4));
        queue.append(record(Sink::OpenPanel, "", 5));   // drops 4
        QCOMPARE(queue.size(), 3);
        queue.append(record(Sink::GlitchTip, "error", 6));   // drops 5, the last analytics
        queue.append(record(Sink::GlitchTip, "error", 7));   // drops 3, the oldest error
        QList<int> left;
        for (const QueuedRecord& r : queue.records())
            left << r.payload.value(QStringLiteral("n")).toInt();
        QCOMPARE(left, (QList<int>{2, 6, 7}));

        // The byte cap works the same way.
        EventQueue small(dir_->path() + QStringLiteral("/small"), {500, 300, 14, 3});
        QVERIFY(small.open());
        for (int i = 0; i < 20; ++i)
            small.append(record(Sink::OpenPanel, "", i));
        QVERIFY(small.size() < 20);
        QCOMPARE(small.records().last().payload.value(QStringLiteral("n")).toInt(), 19);
    }

    // The crash summary (there is at most one) is always the front, so
    // it is the first record sent: in memory, and after a reload of a
    // file whose lines are in the order they were appended.
    void queueKeepsTheCrashInFront() {
        const QString dir = dir_->path() + QStringLiteral("/q");
        auto record = [](Sink sink, const char* level, int n) {
            QueuedRecord r;
            r.sink = sink;
            r.queued = QDateTime::currentDateTimeUtc();
            r.payload = QJsonObject{{QStringLiteral("level"), QLatin1String(level)},
                                    {QStringLiteral("n"), n}};
            return r;
        };
        auto order = [](const EventQueue& queue) {
            QList<int> n;
            for (const QueuedRecord& r : queue.records())
                n << r.payload.value(QStringLiteral("n")).toInt();
            return n;
        };
        {
            EventQueue queue(dir, {});
            QVERIFY(queue.open());
            queue.append(record(Sink::OpenPanel, "", 1));
            queue.append(record(Sink::GlitchTip, "fatal", 2));
            queue.append(record(Sink::OpenPanel, "", 3));
            QVERIFY(queue.front().isCrash());
            QCOMPARE(order(queue), (QList<int>{2, 1, 3}));
            // A later crash replaces it, in front.
            queue.append(record(Sink::GlitchTip, "fatal", 4));
            QCOMPARE(order(queue), (QList<int>{4, 1, 3}));
        }   // no sync(): the file keeps every line, in the order appended
        EventQueue reloaded(dir, {});
        QVERIFY(reloaded.open());
        QVERIFY(reloaded.front().isCrash());
        QCOMPARE(order(reloaded), (QList<int>{4, 1, 3}));
    }

    // A record appended while a request is in flight can push the queue
    // over its cap, and the oldest analytics record it drops may be the
    // one in flight. That reply's outcome then belongs to no record: it
    // must not count an attempt against (or remove) the record that
    // moved up to the front, which was never sent.
    void capsDuringRequestSpareTheNextRecord() {
        Provider provider;
        Config config = testConfig(*stub_, dir_->path());
        config.maxQueued = 2;
        stub_->respond = [](const StubRequest&) { return StubResponse{-1, {}}; };   // a timeout
        auto service = makeService(config, provider);
        service->recordRepoOpened(1, 1, 1);
        service->recordRepoOpened(2, 2, 1);
        service->start();
        QTRY_COMPARE(stub_->requests().size(), 1);
        service->recordRepoOpened(3, 3, 1);   // over the cap: drops 1, in flight

        // The timeout (a Retry) ends the cycle, whose rewrite leaves 2
        // and 3 on disk, neither one tried.
        QTRY_COMPARE(queueLines(config).size(), 2);
        for (int i = 0; i < 2; ++i) {
            const QJsonObject line = queueLines(config).at(i);
            QCOMPARE(line.value(QStringLiteral("attempts")).toInt(), 0);
            QCOMPARE(line.value(QStringLiteral("payload")).toObject()
                         .value(QStringLiteral("payload")).toObject()
                         .value(QStringLiteral("properties")).toObject()
                         .value(QStringLiteral("open_ms")).toInt(),
                     i + 2);
        }
        QCOMPARE(service->queuedCount(), 2);
    }

    void queueLoadRules() {
        const QString dir = dir_->path() + QStringLiteral("/q");
        QVERIFY(QDir().mkpath(dir));
        const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        const QString old =
            QDateTime::currentDateTimeUtc().addDays(-20).toString(Qt::ISODateWithMs);
        QFile file(EventQueue::filePath(dir));
        QVERIFY(file.open(QIODevice::WriteOnly));
        auto line = [](int v, const char* sink, int attempts, const QString& queued) {
            return QStringLiteral("{\"v\":%1,\"sink\":\"%2\",\"attempts\":%3,\"queued\":\"%4\","
                                  "\"payload\":{\"n\":1}}\n")
                .arg(v)
                .arg(QLatin1String(sink))
                .arg(attempts)
                .arg(queued)
                .toUtf8();
        };
        file.write(line(1, "openpanel", 0, now));      // kept
        file.write(line(1, "openpanel", 0, old));      // expired
        file.write(line(2, "openpanel", 0, now));      // unknown version
        file.write(line(1, "elsewhere", 0, now));      // unknown sink
        file.write(line(1, "glitchtip", 3, now));      // used up its attempts
        file.write(line(1, "glitchtip", 1, now));      // kept
        file.write("{not json\n");
        file.write("\n");
        file.write("{\"v\":1,\"sink\":\"openpanel\",\"attempts\":0,\"queued\":\"yesterday\","
                   "\"payload\":{}}\n");
        file.write("{\"v\":1,\"sink\":\"openpanel\",\"attempts\":0,\"queued\":\""
                   + now.toUtf8() + "\",\"payload\":[]}\n");
        file.close();

        EventQueue queue(dir, {500, 1024 * 1024, 14, 3});
        QVERIFY(queue.open());
        QCOMPARE(queue.size(), 2);
        QCOMPARE(queue.records().at(0).sink, Sink::OpenPanel);
        QCOMPARE(queue.records().at(1).sink, Sink::GlitchTip);
        QCOMPARE(queue.records().at(1).attempts, 1);
        QVERIFY(queue.sync());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll().count('\n'), 2);
    }

    // One session per queue: a second process (here, a second queue
    // object) can't take it, sends nothing, and can't discard it.
    void queueLockIsExclusive() {
        Provider provider;
        const Config config = testConfig(*stub_, dir_->path());
        auto first = makeService(config, provider);
        QVERIFY(first->isActive());
        first->recordRepoOpened(1, 1, 1);

        auto second = makeService(config, provider);
        QVERIFY(!second->isActive());
        second->recordLaunch();
        second->start();
        QCOMPARE(second->queuedCount(), 0);

        EventQueue::discard(config.queueDir);
        QVERIFY(QFile::exists(EventQueue::filePath(config.queueDir)));

        first.reset();   // releases the lock
        EventQueue::discard(config.queueDir);
        QVERIFY(!QFile::exists(EventQueue::filePath(config.queueDir)));
        QVERIFY(!QDir(config.queueDir).exists());
        QTest::qWait(100);
        QCOMPARE(stub_->requests().size(), 0);
    }

    // A queue.lock QLockFile can't prove stale with stale time 0: one
    // naming another machine (a home shared between machines, or moved
    // to a new one) or one a crash truncated. Left in place, it kept
    // usage statistics off for good, and the off switch couldn't delete
    // the queue. A live holder's lock is never taken, whatever it says.
    void queueClearsUnprovableStaleLock_data() {
        QTest::addColumn<QByteArray>("content");
        QTest::addColumn<bool>("held");
        QTest::newRow("other machine")
            << QByteArray("999999\nGitBolt\nhost\nffffffff-0000-0000-0000-000000000000\n")
            << false;
        QTest::newRow("empty") << QByteArray() << false;
        QTest::newRow("held, content unprovable") << QByteArray() << true;
    }

    void queueClearsUnprovableStaleLock() {
        QFETCH(QByteArray, content);
        QFETCH(bool, held);
        const QString dir = dir_->path() + QStringLiteral("/q");
        const QString lockPath = dir + QStringLiteral("/queue.lock");
        QVERIFY(writeOneRecordQueue(dir));

        if (held) {
            QLockFile holder(lockPath);
            holder.setStaleLockTime(0);
            QVERIFY(holder.tryLock(0));
#ifdef Q_OS_WIN
            // The holder's handle denies writes, so its content can't
            // be changed; wait until it has settled instead. Qt proves
            // this PID alive, and the removal fails on the open handle.
            QTest::qWait(2100);
#else
            QVERIFY(writeAgedFile(lockPath, content));
#endif
            EventQueue queue(dir, {});
            QVERIFY(!queue.open());
            EventQueue::discard(dir);
            QVERIFY(QFile::exists(EventQueue::filePath(dir)));
            QVERIFY(QFile::exists(lockPath));
            return;
        }

        QVERIFY(writeAgedFile(lockPath, content));
        {
            EventQueue queue(dir, {});
            QVERIFY(queue.open());
            QCOMPARE(queue.size(), 1);
        }

        // The off switch's discard, in a queue of its own.
        const QString other = dir_->path() + QStringLiteral("/discarded");
        QVERIFY(writeOneRecordQueue(other));
        QVERIFY(writeAgedFile(other + QStringLiteral("/queue.lock"), content));
        EventQueue::discard(other);
        QVERIFY(!QFile::exists(EventQueue::filePath(other)));
        QVERIFY(!QDir(other).exists());
    }

    // ---- what is recorded ---------------------------------------------

    void commandCountsAggregate() {
        Provider provider;
        Config config = testConfig(*stub_, dir_->path());
        config.countWindowMs = 200;
        auto service = makeService(config, provider);
        service->start();
        for (int i = 0; i < 3; ++i)
            service->countCommand(QStringLiteral("act.commit"));
        service->countCommand(QStringLiteral("diff.viewUnified"));
        service->countCommand(QStringLiteral("file.recent.0"));
        service->countCommand(QStringLiteral("file.recent.4"));
        QTRY_COMPARE(stub_->requests().size(), 3);

        QHash<QString, int> counts;
        for (const StubRequest& request : stub_->requests()) {
            QCOMPARE(request.json().value(QStringLiteral("payload")).toObject()
                         .value(QStringLiteral("name")).toString(),
                     QStringLiteral("command_used"));
            const QJsonObject props = properties(request);
            counts.insert(props.value(QStringLiteral("command")).toString(),
                          props.value(QStringLiteral("count")).toInt());
        }
        QCOMPARE(counts, (QHash<QString, int>{{QStringLiteral("act.commit"), 3},
                                              {QStringLiteral("diff.viewUnified"), 1},
                                              {QStringLiteral("file.recent"), 2}}));
        // The next window has nothing to say.
        QTest::qWait(300);
        QCOMPARE(stub_->requests().size(), 3);
    }

    void unsafeInputsNeverLeave_data() {
        QTest::addColumn<QString>("input");
        QTest::newRow("path") << "/Users/alice/secret-repo";
        QTest::newRow("home path") << "/home/alice/secret-repo";
        QTest::newRow("branch") << "feature/secret-branch";
        QTest::newRow("space") << "a b secret";
        QTest::newRow("email") << "alice@example.com";
        QTest::newRow("65 chars") << QStringLiteral("secret").repeated(11);
        QTest::newRow("empty") << QString();
    }

    void unsafeInputsNeverLeave() {
        QFETCH(QString, input);
        Provider provider;
        const Config config = testConfig(*stub_, dir_->path());
        auto service = makeService(config, provider);
        service->start();
        service->countCommand(input);
        service->recordError(ErrorKind::OperationFailed, input);
        service->recordError(ErrorKind::OperationFailed,
                             QStringLiteral("push origin feature/secret-branch"));
        service->recordError(ErrorKind::RepoOpenFailed, input);
        service->persistPending();   // counts out now, if any were kept

        // No command was counted; the errors went out sanitized.
        const QList<QJsonObject> lines = queueLines(config);
        for (const QJsonObject& line : lines)
            QCOMPARE(line.value(QStringLiteral("sink")).toString(), QStringLiteral("glitchtip"));
        QVERIFY(!lines.isEmpty());

        service.reset();
        stub_->respond = [](const StubRequest&) { return StubResponse{}; };
        service = makeService(config, provider);
        service->start();
        QTRY_COMPARE(service->queuedCount(), 0);
        QVERIFY(!stub_->requests().isEmpty());
        for (const StubRequest& request : stub_->requests()) {
            const QJsonObject tags = request.json().value(QStringLiteral("tags")).toObject();
            if (tags.contains(QStringLiteral("op")))
                QCOMPARE(tags.value(QStringLiteral("op")).toString(), QStringLiteral("other"));
            if (tags.contains(QStringLiteral("code")))
                QCOMPARE(tags.value(QStringLiteral("code")).toString(), QStringLiteral("other"));
            QByteArray all = request.everything();
            all.replace("gitbolt@1.2.3", "");   // the release names the app, not a person
            for (const char* sentinel : {"alice", "secret", "/Users", "/home", "@", "example.com",
                                         "feature/"})
                QVERIFY2(!all.contains(sentinel), sentinel);
        }
        QCOMPARE(provider.calls, 0);   // GlitchTip events never need the ID
    }

    // Turning usage statistics off and on again doesn't reset the
    // per-session error dedupe: PRIVACY.md says the same error goes at
    // most once per session.
    void errorDedupeSurvivesToggle() {
        Provider provider;
        const Config config = testConfig(*stub_, dir_->path());
        auto service = makeService(config, provider);
        service->start();
        service->recordError(ErrorKind::OperationFailed, QStringLiteral("fetch"));
        QTRY_COMPARE(stub_->requests().size(), 1);
        service->setUserEnabled(false);
        service->setUserEnabled(true);
        service->recordError(ErrorKind::OperationFailed, QStringLiteral("fetch"));
        QTest::qWait(400);
        QCOMPARE(stub_->requests().size(), 1);
        QCOMPARE(service->queuedCount(), 0);
    }

    void glitchTipErrorShape() {
        Provider provider;
        Config config = testConfig(*stub_, dir_->path());
        config.maxErrorsPerSession = 4;
        auto service = makeService(config, provider);
        service->start();
        service->recordError(ErrorKind::OperationFailed, QStringLiteral("fetch"));
        QTRY_COMPARE(stub_->requests().size(), 1);

        const StubRequest& request = stub_->requests().first();
        QCOMPARE(request.path, QByteArray("/api/42/store/"));
        QCOMPARE(request.headerNames(), gitbolt::test::kGlitchTipHeaders);
        const QByteArray auth = request.header("x-sentry-auth");
        QVERIFY(auth.startsWith("Sentry "));
        QVERIFY(auth.contains("sentry_version=7"));
        QVERIFY(auth.contains("sentry_key=" + kGlitchTipKey.toLatin1()));
        QVERIFY(auth.contains("sentry_client=gitbolt/1.2.3"));

        const QJsonObject event = request.json();
        QVERIFY(QRegularExpression(QStringLiteral("^[0-9a-f]{32}$"))
                    .match(event.value(QStringLiteral("event_id")).toString())
                    .hasMatch());
        QCOMPARE(event.value(QStringLiteral("level")).toString(), QStringLiteral("error"));
        QCOMPARE(event.value(QStringLiteral("platform")).toString(), QStringLiteral("native"));
        QCOMPARE(event.value(QStringLiteral("release")).toString(), QStringLiteral("gitbolt@1.2.3"));
        QCOMPARE(event.value(QStringLiteral("environment")).toString(),
                 QStringLiteral("production"));
        QCOMPARE(event.value(QStringLiteral("message")).toString(),
                 QStringLiteral("operation_failed: fetch"));
        QCOMPARE(event.value(QStringLiteral("fingerprint")).toArray(),
                 (QJsonArray{QStringLiteral("operation_failed"), QStringLiteral("fetch")}));
        QCOMPARE(event.value(QStringLiteral("tags")).toObject(),
                 (QJsonObject{{QStringLiteral("kind"), QStringLiteral("operation_failed")},
                              {QStringLiteral("op"), QStringLiteral("fetch")}}));
        QVERIFY(QDateTime::fromString(event.value(QStringLiteral("timestamp")).toString(),
                                      Qt::ISODateWithMs)
                    .isValid());
        QCOMPARE(keySet(event),
                 (QSet<QString>{QStringLiteral("event_id"), QStringLiteral("timestamp"),
                                QStringLiteral("platform"), QStringLiteral("logger"),
                                QStringLiteral("level"), QStringLiteral("release"),
                                QStringLiteral("environment"), QStringLiteral("contexts"),
                                QStringLiteral("message"), QStringLiteral("fingerprint"),
                                QStringLiteral("tags")}));
        QVERIFY(!request.body.contains(kInstallId.toLatin1()));
        QVERIFY(!request.body.contains(deviceIdFor(kInstallId).toLatin1()));

        // Once per (kind, detail) per session, and capped.
        service->recordError(ErrorKind::OperationFailed, QStringLiteral("fetch"));
        service->recordError(ErrorKind::RepoOpenFailed, openErrorName(2));
        service->recordError(ErrorKind::CommitFailed);
        service->recordError(ErrorKind::CommitFailed, QStringLiteral("ignored"));
        service->recordError(ErrorKind::OperationFailed, QStringLiteral("push"));
        service->recordError(ErrorKind::OperationFailed, QStringLiteral("pull"));   // over the cap
        QTRY_COMPARE(stub_->requests().size(), 4);
        QTest::qWait(300);
        QCOMPARE(stub_->requests().size(), 4);

        const QJsonObject open = stub_->requests().at(1).json();
        QCOMPARE(open.value(QStringLiteral("tags")).toObject().value(QStringLiteral("code")).toString(),
                 QStringLiteral("not_found"));
        QCOMPARE(open.value(QStringLiteral("fingerprint")).toArray(),
                 (QJsonArray{QStringLiteral("repo_open_failed"), QStringLiteral("not_found")}));
        const QJsonObject commit = stub_->requests().at(2).json();
        QCOMPARE(commit.value(QStringLiteral("fingerprint")).toArray(),
                 (QJsonArray{QStringLiteral("commit_failed")}));
        QCOMPARE(commit.value(QStringLiteral("tags")).toObject(),
                 (QJsonObject{{QStringLiteral("kind"), QStringLiteral("commit_failed")}}));
        QCOMPARE(provider.calls, 0);
    }

    void crashReportIsQueuedAndSent() {
        Provider provider;
        auto service = makeService(testConfig(*stub_, dir_->path()), provider);
        QString report = crashReport();
        // Refused reports queue nothing, and say so: the crash box only
        // mentions sending for a summary that was queued.
        QVERIFY(!service->queueCrashReport(report.replace(QStringLiteral("Session: user"),
                                                          QStringLiteral("Session: test"))));
        QCOMPARE(service->queuedCount(), 0);
        QVERIFY(service->queueCrashReport(report.replace(QStringLiteral("Session: test"),
                                                         QStringLiteral("Session: user"))));
        QCOMPARE(service->queuedCount(), 1);
        service->start();
        QTRY_COMPARE(stub_->requests().size(), 1);

        const QJsonObject event = stub_->requests().first().json();
        QCOMPARE(event.value(QStringLiteral("level")).toString(), QStringLiteral("fatal"));
        QCOMPARE(event.value(QStringLiteral("release")).toString(), QStringLiteral("gitbolt@0.9.1"));
        QCOMPARE(event.value(QStringLiteral("timestamp")).toString(),
                 QDateTime::fromSecsSinceEpoch(1760000000, QTimeZone::UTC)
                     .toString(Qt::ISODateWithMs));
        const QJsonArray frames = event.value(QStringLiteral("exception")).toObject()
                                      .value(QStringLiteral("values")).toArray().first().toObject()
                                      .value(QStringLiteral("stacktrace")).toObject()
                                      .value(QStringLiteral("frames")).toArray();
        QCOMPARE(frames.size(), 2);
        QCOMPARE(frames.at(1).toObject().value(QStringLiteral("instruction_addr")).toString(),
                 QStringLiteral("0x30"));
        QCOMPARE(frames.at(1).toObject().value(QStringLiteral("function")).toString(),
                 QStringLiteral("<unknown>"));
        QVERIFY(!stub_->requests().first().body.contains(kInstallId.toLatin1()));
        QCOMPARE(provider.calls, 0);
    }

    // The crash summary goes first, and at once: the crash may come
    // back within seconds, and the next crash.log replaces a summary
    // still queued, so a crash loop was never reported while the
    // summary waited out the first-flush delay behind every record an
    // earlier session left.
    void crashSummaryGoesFirstAtOnce() {
        Provider provider;
        Config config = testConfig(*stub_, dir_->path());
        config.firstFlushDelayMs = 10000;   // as shipped
        {
            // An earlier session that quit before sending.
            auto earlier = makeService(config, provider);
            earlier->recordLaunch();
            earlier->recordRepoOpened(1, 1, 1);
            QCOMPARE(earlier->queuedCount(), 2);
        }
        auto service = makeService(config, provider);
        QVERIFY(service->queueCrashReport(crashReport()));
        service->recordLaunch();
        service->start();
        QTRY_VERIFY_WITH_TIMEOUT(!stub_->requests().isEmpty(), 5000);
        const StubRequest& first = stub_->requests().first();
        QCOMPARE(first.path, QByteArray("/api/42/store/"));
        QCOMPARE(first.json().value(QStringLiteral("level")).toString(), QStringLiteral("fatal"));
        // The records behind it go in the same cycle.
        QTRY_COMPARE(stub_->requests().size(), 4);
        QTRY_COMPARE(service->queuedCount(), 0);
    }

    // ---- inert and off sessions ----------------------------------------

    // The SettingOff service: no ID asked for, nothing on disk, nothing
    // sent — until the switch goes on.
    void disabledServiceTouchesNothing() {
        Provider provider;
        const Config config = testConfig(*stub_, dir_->path());
        auto service = makeService(config, provider, false);
        QVERIFY(!service->isActive());
        service->start();
        service->recordLaunch();
        service->countCommand(QStringLiteral("act.commit"));
        service->recordRepoOpened(1, 1, 1);
        service->recordError(ErrorKind::OperationFailed, QStringLiteral("fetch"));
        QVERIFY(!service->queueCrashReport(QStringLiteral("anything")));
        service->persistPending();
        QTest::qWait(100);
        QCOMPARE(provider.calls, 0);
        QVERIFY(!QDir(config.queueDir).exists());
        QCOMPARE(stub_->requests().size(), 0);

        auto again = makeService(config, provider, false);
        again->start();
        again->setUserEnabled(true);
        again->recordLaunch();
        QTRY_COMPARE(stub_->requests().size(), 1);
        QCOMPARE(provider.calls, 1);
    }

    // A hand-edited install ID is never sent as a profile ID; the ID is
    // asked for once.
    void invalidInstallIdSendsNoAnalytics() {
        Provider provider;
        provider.id = QStringLiteral("alice@example.com");
        auto service = makeService(testConfig(*stub_, dir_->path()), provider);
        const Config config = testConfig(*stub_, dir_->path());
        service->start();
        service->recordLaunch();
        service->recordRepoOpened(1, 1, 1);
        service->countCommand(QStringLiteral("act.commit"));
        QCOMPARE(service->queuedCount(), 0);
        service->persistPending();
        QVERIFY(!QFile::exists(EventQueue::filePath(config.queueDir)));
        QTest::qWait(100);
        QCOMPARE(stub_->requests().size(), 0);
        QCOMPARE(provider.calls, 1);
    }
};

QTEST_GUILESS_MAIN(TestTelemetry)
#include "TestTelemetry.moc"
