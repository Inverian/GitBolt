//
// TestCrashLogParser — crash.log (util::CrashHandler) to GlitchTip
// event (telemetry::CrashLogParser + glitchTipCrash):
//
//   - refusals, one fixture each: unofficial build, environment
//     opt-out, test session, format 1, bad version; and every header
//     combination through CrashHandler::renderHeader itself, with the
//     values injected (the real environment under ctest and CI would
//     otherwise decide)
//   - reduction: handler frames dropped by position, oldest frame
//     first, GitBolt's frames "<unknown>" with their module offset,
//     unknown modules "<other>" with nothing else, no path or user
//     name anywhere in the event
//   - the fingerprint: stable for one build, per release
//   - a death test (POSIX): a helper installs the real handler and
//     dies on SIGSEGV; the crash.log it wrote parses, with the handler
//     and trampoline frames gone, GitBolt's frames keep their module
//     when it was started through a link of another name (an
//     AppImage's AppRun), and a fault's newest frame is the faulting
//     function itself; a test session's crash leaves the user's
//     crash.log alone, and on macOS stays out of the system's crash
//     reporter
//
// Synthetic reports pin their "OS:" line (CrashHeader.h): the parser's
// frame rules follow it, and the Windows runner would otherwise write
// its own. Fixtures are read as text: a Windows checkout has CRLF.
//

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>

#include "CrashHeader.h"
#include "telemetry/CrashLogParser.h"
#include "telemetry/EventSchema.h"
#include "util/CrashHandler.h"

#include <optional>

using gitbolt::telemetry::CrashEvent;
using gitbolt::telemetry::CrashFrame;
using gitbolt::telemetry::CrashLogParser;
using gitbolt::test::crashHeader;
using gitbolt::test::withCrashOs;
using gitbolt::util::CrashHandler;

namespace {

// CrashLogParser demangles where the compiler has <cxxabi.h>. MSVC has
// none, and the parser then drops every "_Z" name rather than send it
// raw (Windows reports carry undecorated names anyway).
#if __has_include(<cxxabi.h>)
constexpr bool kDemangles = true;
#else
constexpr bool kDemangles = false;
#endif

// `name` where the parser can demangle, and no name at all elsewhere.
QString demangled(const char* name) {
    return kDemangles ? QString::fromLatin1(name) : QString();
}

QString fixture(const QString& name) {
    QFile file(QStringLiteral(GITBOLT_CRASH_FIXTURES "/") + name);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(file.readAll());
}

// A report as the handler writes it: header, signal, epoch, frames.
QString report(const QByteArray& header, const QStringList& frames,
               const QString& signal = QStringLiteral("SIGSEGV (11)"),
               qint64 epoch = 1760000000) {
    return QString::fromUtf8(header)
           + QStringLiteral("Signal: %1\nEpoch: %2\n\n--- Stack Trace ---\n").arg(signal).arg(epoch)
           + frames.join(QLatin1Char('\n')) + QStringLiteral("\n\n=== End of Report ===\n");
}

QByteArray officialHeader(const char* version = "0.1.5") {
    return crashHeader(version, true, false, false);
}

QStringList functions(const CrashEvent& event) {
    QStringList names;
    for (const CrashFrame& frame : event.frames)
        names << frame.function;
    return names;
}

QByteArray eventJson(const CrashEvent& event) {
    return QJsonDocument(gitbolt::telemetry::glitchTipCrash(event)).toJson(QJsonDocument::Compact);
}

} // namespace

class TestCrashLogParser : public QObject {
    Q_OBJECT

private slots:
    void refusedFixtures_data() {
        QTest::addColumn<QString>("name");
        QTest::newRow("unofficial build") << "unofficial.log";
        QTest::newRow("environment opt-out") << "opt-out.log";
        QTest::newRow("test session") << "test-session.log";
        QTest::newRow("format 1") << "format1.log";
        QTest::newRow("bad version") << "bad-version.log";
    }

    void refusedFixtures() {
        QFETCH(QString, name);
        const QString log = fixture(name);
        QVERIFY(!log.isEmpty());
        QVERIFY(!CrashLogParser::parse(log));
        // Each differs from an accepted one in that one respect.
        QVERIFY(CrashLogParser::parse(fixture(QStringLiteral("macos.log"))));
    }

    // Through the real header writer: only official + none + user.
    void headerRoundTrip_data() {
        QTest::addColumn<bool>("official");
        QTest::addColumn<bool>("optOut");
        QTest::addColumn<bool>("testSession");
        QTest::addColumn<bool>("accepted");
        QTest::newRow("official user") << true << false << false << true;
        QTest::newRow("unofficial") << false << false << false << false;
        QTest::newRow("opted out") << true << true << false << false;
        QTest::newRow("test session") << true << false << true << false;
        QTest::newRow("all three") << false << true << true << false;
    }

    void headerRoundTrip() {
        QFETCH(bool, official);
        QFETCH(bool, optOut);
        QFETCH(bool, testSession);
        QFETCH(bool, accepted);
        const QByteArray header = crashHeader("0.1.0", official, optOut, testSession);
        QVERIFY(header.startsWith("=== GitBolt Crash Report ===\nFormat: 2\nApp version: 0.1.0\n"));
        QVERIFY(header.contains(official ? "\nBuild: official\n" : "\nBuild: unofficial\n"));
        QVERIFY(header.contains(optOut ? "\nOpt-out: env\n" : "\nOpt-out: none\n"));
        QVERIFY(header.contains(testSession ? "\nSession: test\n" : "\nSession: user\n"));

        const std::optional<CrashEvent> event = CrashLogParser::parse(report(
            header, {QStringLiteral("#0 GitBolt +0x10 ?"),
                     QStringLiteral("#1 libsystem_platform.dylib +0x20 _sigtramp+0x8"),
                     QStringLiteral("#2 GitBolt +0x30 ?")}));
        QCOMPARE(event.has_value(), accepted);
        if (!accepted)
            return;
        QCOMPARE(event->release, QStringLiteral("0.1.0"));
        QCOMPARE(event->qtVersion, QString::fromLatin1(qVersion()));
        QCOMPARE(event->frames.size(), 1);
        QCOMPARE(event->frames.first().offset, std::optional<quint64>(0x30));
    }

    void noVersionIsUnknownAndRefused() {
        QVERIFY(CrashHandler::renderHeader(nullptr, true, false, false)
                    .contains("\nApp version: unknown\n"));
        QVERIFY(!CrashLogParser::parse(
            report(CrashHandler::renderHeader("", true, false, false),
                   {QStringLiteral("#2 GitBolt +0x30 ?")})));
    }

    void acceptedFixtures_data() {
        QTest::addColumn<QString>("name");
        QTest::addColumn<QString>("signal");
        QTest::addColumn<int>("signalNumber");
        QTest::addColumn<QString>("os");
        QTest::addColumn<QString>("osVersion");
        QTest::addColumn<QString>("arch");
        QTest::addColumn<qint64>("epoch");
        QTest::addColumn<int>("frameCount");
        QTest::addColumn<QString>("crashPackage");   // the newest frame's
        QTest::addColumn<quint64>("crashOffset");
        QTest::addColumn<QString>("oldestFunction");
        QTest::newRow("macOS") << "macos.log" << "SIGSEGV" << 11 << "macOS" << "15.6" << "arm64"
                               << qint64(1760000000) << 14 << "GitBolt" << quint64(0x5c3f0)
                               << "start";
        QTest::newRow("Linux") << "linux.log" << "SIGABRT" << 6 << "Linux" << "ubuntu 24.04"
                               << "x86_64" << qint64(1760000123) << 12 << "libc.so.6"
                               << quint64(0x9eb2c) << "__libc_start_call_main";
        // #0 the handler, #1-#5 the CRT and the loader dispatching the
        // exception, up to KiUserExceptionDispatcher.
        QTest::newRow("Windows") << "windows.log" << "SIGSEGV" << 11 << "Windows" << "11"
                                 << "x86_64" << qint64(1760000456) << 6 << "GitBolt.exe"
                                 << quint64(0x51a40) << "RtlUserThreadStart";
        // An unhandled C++ exception: #0 the handler, #1-#3 abort() from
        // terminate(), #4 the CRT's unhandled-exception filter (inside
        // GitBolt.exe), #5-#9 the dispatch, #10-#12 the throw itself.
        QTest::newRow("Windows C++ exception")
            << "windows-exception.log" << "SIGABRT" << 22 << "Windows" << "11" << "x86_64"
            << qint64(1760000789) << 5 << "GitBolt.exe" << quint64(0x51a40)
            << "RtlUserThreadStart";
    }

    void acceptedFixtures() {
        QFETCH(QString, name);
        QFETCH(QString, signal);
        QFETCH(int, signalNumber);
        QFETCH(QString, os);
        QFETCH(QString, osVersion);
        QFETCH(QString, arch);
        QFETCH(qint64, epoch);
        QFETCH(int, frameCount);
        QFETCH(QString, crashPackage);
        QFETCH(quint64, crashOffset);
        QFETCH(QString, oldestFunction);

        const std::optional<CrashEvent> event = CrashLogParser::parse(fixture(name));
        QVERIFY(event);
        // From the report, never from the running app.
        QCOMPARE(event->release, QStringLiteral("0.1.5"));
        QCOMPARE(event->at, QDateTime::fromSecsSinceEpoch(epoch, QTimeZone::UTC));
        QCOMPARE(event->signal, signal);
        QCOMPARE(event->signalNumber, signalNumber);
        QCOMPARE(event->os, os);
        QCOMPARE(event->osVersion, osVersion);
        QCOMPARE(event->arch, arch);
        QCOMPARE(event->qtVersion, QStringLiteral("6.10.0"));

        // Handler frames dropped, oldest first.
        QCOMPARE(event->frames.size(), frameCount);
        QCOMPARE(event->frames.last().package, crashPackage);
        QCOMPARE(event->frames.last().offset, std::optional<quint64>(crashOffset));
        QCOMPARE(event->frames.first().function, oldestFunction);
        QVERIFY(!functions(*event).contains(QStringLiteral("_sigtramp")));
        QVERIFY(!functions(*event).contains(QStringLiteral("KiUserExceptionDispatcher")));

        // GitBolt's own frames: "<unknown>", with the module offset.
        // Everything else from an unknown module: "<other>", nothing more.
        int inApp = 0;
        for (const CrashFrame& frame : event->frames) {
            if (frame.inApp) {
                ++inApp;
                QCOMPARE(frame.function, QStringLiteral("<unknown>"));
                QVERIFY(frame.offset);
                QVERIFY(QStringList({QStringLiteral("GitBolt"), QStringLiteral("gitbolt"),
                                     QStringLiteral("GitBolt.exe")})
                            .contains(frame.package));
            } else if (frame.package == QLatin1String("<other>")) {
                QVERIFY(frame.function.isEmpty());
                QVERIFY(!frame.offset);
            } else {
                QVERIFY(frame.offset);
            }
        }
        QVERIFY(inApp > 0);

        // Nothing from a path, a home directory, a user name or a
        // third-party module reaches the event.
        const QByteArray json = eventJson(*event);
        for (const char* sentinel : {"/", "\\", "Users", "home", "bob", "alice", "Alice",
                                     "Secret", "AppRun", "foohook", "ibus"})
            QVERIFY2(!json.contains(sentinel), sentinel);
    }

    void systemFramesKeepCleanNames() {
        const std::optional<CrashEvent> mac = CrashLogParser::parse(fixture(QStringLiteral("macos.log")));
        QVERIFY(mac);
        const QStringList names = functions(*mac);
        // Mangled in the fixture: demangled, or not sent at all.
        if (kDemangles) {
            QVERIFY(names.contains(QStringLiteral("QMetaObject::activate")));
            QVERIFY(names.contains(QStringLiteral("QApplicationPrivate::notify_helper")));
            QVERIFY(names.contains(QStringLiteral("QEventLoop::exec")));
        }
        for (const QString& name : names)
            QVERIFY2(!name.startsWith(QLatin1String("_Z")) && !name.startsWith(QLatin1String("__Z")),
                     qPrintable(name));
        QVERIFY(names.contains(QStringLiteral("-[NSApplication run]")));
        QVERIFY(names.contains(
            QStringLiteral("__CFRUNLOOP_IS_CALLING_OUT_TO_A_SOURCE0_PERFORM_FUNCTION__")));
        // A stripped in-app name, however plausible, is never sent.
        QVERIFY(!names.contains(QStringLiteral("main")));
        QVERIFY(!eventJson(*mac).contains("MainWindow"));

        const std::optional<CrashEvent> linuxEvent = CrashLogParser::parse(fixture(QStringLiteral("linux.log")));
        QVERIFY(linuxEvent);
        QCOMPARE(functions(*linuxEvent).contains(QStringLiteral("std::terminate")), kDemangles);
        QVERIFY(functions(*linuxEvent).contains(QStringLiteral("abort")));
        // The trampoline (#1) had no name; dropped by position anyway.
        QCOMPARE(linuxEvent->frames.last().function, QStringLiteral("pthread_kill"));
    }

    void frameLineVariants() {
        const std::optional<CrashEvent> event = CrashLogParser::parse(report(
            officialHeader(),
            {QStringLiteral("#0 GitBolt +0x10 ?"), QStringLiteral("#1 libsystem_platform.dylib +0x20 ?"),
             QStringLiteral("#2 ? 0x7fff12345678"),
             QStringLiteral("#3 QtCore +0x40 _ZN12_GLOBAL__N_16helperEi+0x4"),
             QStringLiteral("#4 QtCore +0x50 _ZlsR6QDebugPKc+0x4"),
             QStringLiteral("#5 QtCore +0x60 not a frame line"),
             QStringLiteral("garbage that is not a frame"),
             QStringLiteral("#6 QtCore +0x70 weird$name+0x1"),
             QStringLiteral("#7 QtCore +0x80 ") + QString(250, u'a') + QStringLiteral("+0x1"),
             QStringLiteral("#8 QtCore +0x90 _ZN7QObject5eventEP6QEvent+0x4"),
             QStringLiteral("#9 QtCore +0x98 _ZN99NotMangledRight+0x4"),
             QStringLiteral("#10 Some_Helper_App +0xa0 helper+0x1")}));
        QVERIFY(event);
        // Newest last; #5 had no symbol suffix, the garbage line none at all.
        QList<QPair<QString, QString>> got;
        for (const CrashFrame& frame : event->frames)
            got.append({frame.package, frame.function});
        const QList<QPair<QString, QString>> expected = {
            {QStringLiteral("<other>"), QString()},
            {QStringLiteral("QtCore"), QString()},   // "_Z…" that doesn't demangle
            {QStringLiteral("QtCore"), demangled("QObject::event")},
            {QStringLiteral("QtCore"), QString()},   // > 200 characters
            {QStringLiteral("QtCore"), QString()},   // '$' isn't allowed
            {QStringLiteral("QtCore"), QString()},   // "not a frame line": no "+0x"
            {QStringLiteral("QtCore"), demangled("operator<<")},
            {QStringLiteral("QtCore"), demangled("(anonymous namespace)::helper")},
            {QStringLiteral("<other>"), QString()},  // no module at all
        };
        QCOMPARE(got, expected);
    }

    void windowsAbortDropsCrtFrames() {
        const std::optional<CrashEvent> event = CrashLogParser::parse(report(
            withCrashOs(officialHeader(), "windows 11"),
            {QStringLiteral("#0 GitBolt.exe +0x100 ?"), QStringLiteral("#1 ucrtbase.dll +0x200 raise+0x1"),
             QStringLiteral("#2 ucrtbase.dll +0x300 abort+0x1"), QStringLiteral("#3 GitBolt.exe +0x400 ?"),
             QStringLiteral("#4 KERNEL32.DLL +0x500 BaseThreadInitThunk+0x1")},
            QStringLiteral("SIGABRT (22)")));
        QVERIFY(event);
        QCOMPARE(event->os, QStringLiteral("Windows"));
        QCOMPARE(event->frames.size(), 2);
        QCOMPARE(event->frames.last().package, QStringLiteral("GitBolt.exe"));
        QCOMPARE(event->frames.last().offset, std::optional<quint64>(0x400));
    }

    // An unhandled C++ exception reaches the handler through the CRT's
    // unhandled-exception filter, which is in GitBolt.exe: it used to
    // pass for the crash point, with the throw's frames kept above the
    // thrower. The crash groups with the thrower, as a fault there would.
    void windowsCxxExceptionDropsThrowMachinery() {
        const std::optional<CrashEvent> event =
            CrashLogParser::parse(fixture(QStringLiteral("windows-exception.log")));
        QVERIFY(event);
        const QStringList machinery = {QStringLiteral("terminate"),
                                       QStringLiteral("UnhandledExceptionFilter"),
                                       QStringLiteral("RtlDispatchException"),
                                       QStringLiteral("RtlRaiseException"),
                                       QStringLiteral("RaiseException"),
                                       QStringLiteral("_CxxThrowException")};
        for (const CrashFrame& frame : event->frames) {
            QVERIFY(frame.offset != std::optional<quint64>(0x2f9a4));   // the CRT's filter
            QVERIFY2(!machinery.contains(frame.function), qPrintable(frame.function));
        }

        const std::optional<CrashEvent> there = CrashLogParser::parse(report(
            withCrashOs(officialHeader(), "windows 11"),
            {QStringLiteral("#0 GitBolt.exe +0x1c2a0 ?"), QStringLiteral("#1 GitBolt.exe +0x51a40 ?"),
             QStringLiteral("#2 GitBolt.exe +0x52b10 ?"),
             QStringLiteral("#3 Qt6Core.dll +0x2b1c0 QMetaObject::activate+0x40")},
            QStringLiteral("SIGABRT (22)")));
        QVERIFY(there);
        QCOMPARE(event->fingerprint, there->fingerprint);
    }

    void keepsTheNewest64Frames() {
        QStringList frames;
        for (int i = 0; i < 80; ++i)
            frames << QStringLiteral("#%1 QtCore +0x%2 ?").arg(i).arg(i, 0, 16);
        const std::optional<CrashEvent> event = CrashLogParser::parse(report(officialHeader(), frames));
        QVERIFY(event);
        QCOMPARE(event->frames.size(), 64);
        QCOMPARE(event->frames.last().offset, std::optional<quint64>(2));   // #2, the crash point
        QCOMPARE(event->frames.first().offset, std::optional<quint64>(65));
    }

    void fingerprint() {
        const QString log = fixture(QStringLiteral("macos.log"));
        const std::optional<CrashEvent> a = CrashLogParser::parse(log);
        const std::optional<CrashEvent> b = CrashLogParser::parse(log);
        QVERIFY(a && b);
        QCOMPARE(a->fingerprint, b->fingerprint);
        QCOMPARE(a->fingerprint.size(), 3);
        QCOMPARE(a->fingerprint.at(0), QStringLiteral("crash"));
        QCOMPARE(a->fingerprint.at(1), QStringLiteral("SIGSEGV"));
        QVERIFY(QRegularExpression(QStringLiteral("^gitbolt@0\\.1\\.5:[0-9a-f]{16}$"))
                    .match(a->fingerprint.at(2))
                    .hasMatch());

        // Per release: offsets mean nothing across builds.
        QString other = log;
        other.replace(QStringLiteral("App version: 0.1.5"), QStringLiteral("App version: 0.1.6"));
        const std::optional<CrashEvent> c = CrashLogParser::parse(other);
        QVERIFY(c);
        QVERIFY(c->fingerprint.at(2).startsWith(QLatin1String("gitbolt@0.1.6:")));
        QCOMPARE(c->fingerprint.at(2).mid(14), a->fingerprint.at(2).mid(14));

        // A different crash site, a different group: any of the first
        // three GitBolt frames (#2, #3, #14 here). Other modules'
        // frames don't matter.
        for (const char* frame : {"#2 GitBolt +0x5c3f0", "#14 GitBolt +0x3e10"}) {
            QString elsewhere = log;
            elsewhere.replace(QLatin1String(frame), QLatin1String(frame) + QLatin1Char('4'));
            QVERIFY(CrashLogParser::parse(elsewhere)->fingerprint != a->fingerprint);
        }
        QString qt = log;
        qt.replace(QStringLiteral("#4 QtCore +0x2d1a40"), QStringLiteral("#4 QtCore +0x2d1a44"));
        QCOMPARE(CrashLogParser::parse(qt)->fingerprint, a->fingerprint);
    }

    void crashEventShape() {
        const std::optional<CrashEvent> crash = CrashLogParser::parse(fixture(QStringLiteral("linux.log")));
        QVERIFY(crash);
        const QJsonObject event = gitbolt::telemetry::glitchTipCrash(*crash);
        const QStringList keys = event.keys();
        QCOMPARE(QSet<QString>(keys.cbegin(), keys.cend()),
                 (QSet<QString>{QStringLiteral("event_id"), QStringLiteral("timestamp"),
                                QStringLiteral("platform"), QStringLiteral("logger"),
                                QStringLiteral("level"), QStringLiteral("release"),
                                QStringLiteral("environment"), QStringLiteral("contexts"),
                                QStringLiteral("exception"), QStringLiteral("fingerprint"),
                                QStringLiteral("tags")}));
        QCOMPARE(event.value(QStringLiteral("level")).toString(), QStringLiteral("fatal"));
        QCOMPARE(event.value(QStringLiteral("platform")).toString(), QStringLiteral("native"));
        QCOMPARE(event.value(QStringLiteral("release")).toString(), QStringLiteral("gitbolt@0.1.5"));
        QCOMPARE(event.value(QStringLiteral("timestamp")).toString(),
                 QDateTime::fromSecsSinceEpoch(1760000123, QTimeZone::UTC).toString(Qt::ISODateWithMs));
        QCOMPARE(event.value(QStringLiteral("tags")).toObject(),
                 (QJsonObject{{QStringLiteral("kind"), QStringLiteral("crash")}}));
        const QJsonObject contexts = event.value(QStringLiteral("contexts")).toObject();
        QCOMPARE(contexts.value(QStringLiteral("os")).toObject().value(QStringLiteral("name")).toString(),
                 QStringLiteral("Linux"));
        QCOMPARE(contexts.value(QStringLiteral("device")).toObject().value(QStringLiteral("arch")).toString(),
                 QStringLiteral("x86_64"));
        QCOMPARE(contexts.value(QStringLiteral("runtime")).toObject(),
                 (QJsonObject{{QStringLiteral("name"), QStringLiteral("Qt")},
                              {QStringLiteral("version"), QStringLiteral("6.10.0")}}));

        const QJsonObject exception = event.value(QStringLiteral("exception")).toObject()
                                          .value(QStringLiteral("values")).toArray().first().toObject();
        QCOMPARE(exception.value(QStringLiteral("type")).toString(), QStringLiteral("SIGABRT"));
        const QJsonObject mechanism = exception.value(QStringLiteral("mechanism")).toObject();
        QCOMPARE(mechanism.value(QStringLiteral("type")).toString(), QStringLiteral("signalhandler"));
        QCOMPARE(mechanism.value(QStringLiteral("handled")).toBool(true), false);
        QCOMPARE(mechanism.value(QStringLiteral("meta")).toObject().value(QStringLiteral("signal"))
                     .toObject().value(QStringLiteral("number")).toInt(),
                 6);

        const QJsonArray frames = exception.value(QStringLiteral("stacktrace")).toObject()
                                      .value(QStringLiteral("frames")).toArray();
        QCOMPARE(frames.size(), crash->frames.size());
        bool sawInApp = false;
        bool sawOther = false;
        for (const QJsonValue& value : frames) {
            const QJsonObject frame = value.toObject();
            const QString package = frame.value(QStringLiteral("package")).toString();
            if (package == QLatin1String("<other>")) {
                sawOther = true;
                QCOMPARE(frame, (QJsonObject{{QStringLiteral("package"), package},
                                             {QStringLiteral("in_app"), false}}));
                continue;
            }
            QVERIFY(QRegularExpression(QStringLiteral("^0x[0-9a-f]+$"))
                        .match(frame.value(QStringLiteral("instruction_addr")).toString())
                        .hasMatch());
            QCOMPARE(frame.value(QStringLiteral("image_addr")).toString(), QStringLiteral("0x0"));
            if (frame.value(QStringLiteral("in_app")).toBool()) {
                sawInApp = true;
                QCOMPARE(frame.value(QStringLiteral("function")).toString(), QStringLiteral("<unknown>"));
            }
        }
        QVERIFY(sawInApp);
        QVERIFY(sawOther);
    }

    void garbledReportsRefused() {
        const QString good = fixture(QStringLiteral("macos.log"));
        QVERIFY(CrashLogParser::parse(good));
        auto without = [&good](const QString& line) {
            QString log = good;
            log.remove(line + QLatin1Char('\n'));
            return log;
        };
        QVERIFY(!CrashLogParser::parse(QString()));
        QVERIFY(!CrashLogParser::parse(good.mid(1)));                                   // banner
        QVERIFY(!CrashLogParser::parse(without(QStringLiteral("--- Stack Trace ---"))));
        QVERIFY(!CrashLogParser::parse(without(QStringLiteral("Epoch: 1760000000"))));
        QVERIFY(!CrashLogParser::parse(without(QStringLiteral("Signal: SIGSEGV (11)"))));
        QVERIFY(!CrashLogParser::parse(without(QStringLiteral("Session: user"))));
        QString twice = good;
        twice.replace(QStringLiteral("Session: user\n"), QStringLiteral("Session: user\nSession: user\n"));
        QVERIFY(!CrashLogParser::parse(twice));
        QString signal = good;
        signal.replace(QStringLiteral("SIGSEGV (11)"), QStringLiteral("SIGKILL (9)"));
        QVERIFY(!CrashLogParser::parse(signal));
        // A frame line can't pass for a header line.
        QString late = without(QStringLiteral("Session: user"));
        late.replace(QStringLiteral("=== End of Report ==="),
                     QStringLiteral("Session: user\n=== End of Report ==="));
        QVERIFY(!CrashLogParser::parse(late));
    }

    // The real writer: a helper process installs the handler and dies
    // on SIGSEGV with HOME in a temporary directory.
    void realCrashLogParses_data() {
        QTest::addColumn<QString>("launchedAs");
        QTest::addColumn<bool>("fault");
        QTest::newRow("by its own name") << QString() << false;
        // glibc's dladdr names the main program by argv[0], and an
        // AppImage runs GitBolt as AppRun or by the .AppImage's name.
        QTest::newRow("through a link of another name")
            << QStringLiteral("GitBolt-0.1.5-x86_64.AppImage") << false;
        // A real fault, in a leaf function of GitBolt's own: that
        // function must be the newest frame. macOS's backtrace() walks
        // frame pointers and never has the interrupted PC, so the
        // caller used to pass for the crash point.
        QTest::newRow("a fault in a leaf function") << QString() << true;
    }

    void realCrashLogParses() {
#ifndef GITBOLT_CRASH_HELPER
        QSKIP("POSIX only: the Windows handler is a different code path");
#else
        QFETCH(QString, launchedAs);
        QFETCH(bool, fault);
        QTemporaryDir home;
        QVERIFY(home.isValid());
        QString program = QStringLiteral(GITBOLT_CRASH_HELPER);
        if (!launchedAs.isEmpty()) {
            const QString link = home.filePath(launchedAs);
            QVERIFY(QFile::link(program, link));
            program = link;
        }
        QProcess helper;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("HOME"), home.path());
        helper.setProcessEnvironment(env);
        helper.start(program, fault ? QStringList{QStringLiteral("fault")} : QStringList());
        QVERIFY(helper.waitForFinished(60000));
        QCOMPARE(helper.exitStatus(), QProcess::CrashExit);

        QFile file(home.filePath(QStringLiteral(".gitbolt/crash.log")));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString log = QString::fromUtf8(file.readAll());
        QVERIFY2(log.startsWith(QString::fromUtf8(CrashHandler::renderHeader("0.1.0", true, false, false))),
                 qPrintable(log));
        QVERIFY2(log.contains(QStringLiteral("\nSignal: SIGSEGV (11)\n")), qPrintable(log));

        // Every stack line is in the parser's format, numbered from 0.
        static const QRegularExpression line(QStringLiteral(
            "^#(\\d+) (?:(\\S+) \\+0x([0-9a-f]+) (?:.+\\+0x[0-9a-f]+|\\?)|\\? 0x[0-9a-f]+)$"));
        const qsizetype begin = log.indexOf(QStringLiteral("--- Stack Trace ---\n"));
        const qsizetype end = log.indexOf(QStringLiteral("\n\n=== End of Report ==="));
        QVERIFY(begin > 0 && end > begin);
        const QStringList lines = log.mid(begin + 20, end - begin - 20).split(QLatin1Char('\n'));
        QVERIFY2(lines.size() >= 4, qPrintable(log));
        QStringList modules;
        QList<quint64> offsets;
        for (qsizetype i = 0; i < lines.size(); ++i) {
            const QRegularExpressionMatch m = line.match(lines.at(i));
            QVERIFY2(m.hasMatch(), qPrintable(lines.at(i)));
            QCOMPARE(m.captured(1).toInt(), int(i));
            modules << m.captured(2);
            offsets << m.captured(3).toULongLong(nullptr, 16);
        }
        // #0 is the handler, in the crashing binary, under the binary's
        // own file name however it was started; #1 the kernel's
        // trampoline, in the C library.
        QCOMPARE(modules.at(0), QStringLiteral("gitbolt"));
        QVERIFY(!modules.at(1).isEmpty() && modules.at(1) != QLatin1String("gitbolt"));

        const std::optional<CrashEvent> event = CrashLogParser::parse(log);
        QVERIFY2(event, qPrintable(log));
        QCOMPARE(event->release, QStringLiteral("0.1.0"));
        QCOMPARE(event->signal, QStringLiteral("SIGSEGV"));
        QCOMPARE(event->signalNumber, 11);
        QCOMPARE(event->frames.size(), lines.size() - 2);
        // The newest frame left is #2, where raise() was running, or
        // the faulting instruction.
        const CrashFrame& top = event->frames.last();
        if (top.package != QLatin1String("<other>")) {
            QCOMPARE(top.package, modules.at(2));
            QCOMPARE(top.offset, std::optional<quint64>(offsets.at(2)));
        }
        if (fault) {
            const QRegularExpressionMatch start =
                QRegularExpression(QStringLiteral("^faultNow \\+0x([0-9a-f]+)$"),
                                   QRegularExpression::MultilineOption)
                    .match(QString::fromUtf8(helper.readAllStandardOutput()));
            QVERIFY(start.hasMatch());
            const quint64 faultNow = start.captured(1).toULongLong(nullptr, 16);
            QVERIFY2(top.inApp && top.offset && *top.offset >= faultNow
                         && *top.offset < faultNow + 0x100,
                     qPrintable(log));
        }
        // crashNow, crashSoon and main, as the app's own: no names.
        int inApp = 0;
        for (const CrashFrame& frame : event->frames) {
            if (!frame.inApp)
                continue;
            ++inApp;
            QCOMPARE(frame.function, QStringLiteral("<unknown>"));
            QVERIFY(frame.offset != std::optional<quint64>(offsets.at(0)));   // not the handler
        }
        QVERIFY2(inApp >= 1, qPrintable(log));
        QVERIFY(!eventJson(*event).contains(home.path().toUtf8()));
#endif
    }

    // A test session's crash goes to crash-test.log. crash.log is the
    // user's (a bridge run outside the e2e harness keeps the real
    // HOME), and the handler truncates the file it writes: a report of
    // theirs still waiting for their next launch used to be replaced by
    // one that launch then refused to send.
    void testSessionCrashKeepsUsersLog() {
#ifndef GITBOLT_CRASH_HELPER
        QSKIP("POSIX only: the Windows handler is a different code path");
#else
        QTemporaryDir home;
        QVERIFY(home.isValid());
        QVERIFY(QDir(home.path()).mkpath(QStringLiteral(".gitbolt")));
        const QString usersLog = home.filePath(QStringLiteral(".gitbolt/crash.log"));
        const QByteArray users = fixture(QStringLiteral("macos.log")).toUtf8();
        QVERIFY(!users.isEmpty());
        {
            QFile file(usersLog);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(users), users.size());
        }

        QProcess helper;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("HOME"), home.path());
        helper.setProcessEnvironment(env);
        helper.start(QStringLiteral(GITBOLT_CRASH_HELPER), {QStringLiteral("test-session")});
        QVERIFY(helper.waitForFinished(60000));
        QCOMPARE(helper.exitStatus(), QProcess::CrashExit);

        QFile kept(usersLog);
        QVERIFY(kept.open(QIODevice::ReadOnly));
        QCOMPARE(kept.readAll(), users);

        QFile testLog(home.filePath(QStringLiteral(".gitbolt/crash-test.log")));
        QVERIFY(testLog.open(QIODevice::ReadOnly));
        const QString log = QString::fromUtf8(testLog.readAll());
        QVERIFY2(log.startsWith(QString::fromUtf8(CrashHandler::renderHeader("0.1.0", true, false, true))),
                 qPrintable(log));
        QVERIFY(log.contains(QStringLiteral("\n--- Stack Trace ---\n#0 ")));
        QVERIFY(!CrashLogParser::parse(log));
#endif
    }

    // macOS: a test session's crash (the bridge's, CI's, a death test's)
    // went to ReportCrash too, which put the "GitBolt quit unexpectedly"
    // box on the user's screen and a GitBolt-*.ips in their
    // DiagnosticReports, crash-test.log written already. The handler of
    // a test session takes EXC_CRASH itself; a user's session leaves it
    // to the system.
    void testSessionKeepsTheCrashReporterOut() {
#if !defined(GITBOLT_CRASH_HELPER) || !defined(Q_OS_MACOS)
        QSKIP("macOS only: its crash reporter is what is kept out");
#else
        QTemporaryDir home;
        QVERIFY(home.isValid());
        const auto crashPort = [&](const QStringList& args) {
            QProcess helper;
            QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
            env.insert(QStringLiteral("HOME"), home.path());
            helper.setProcessEnvironment(env);
            helper.start(QStringLiteral(GITBOLT_CRASH_HELPER), args);
            if (!helper.waitForFinished(60000) || helper.exitStatus() != QProcess::NormalExit
                || helper.exitCode() != 0)
                return QStringLiteral("<helper failed>");
            return QString::fromUtf8(helper.readAllStandardOutput()).trimmed();
        };
        QCOMPARE(crashPort({QStringLiteral("test-session"), QStringLiteral("check-ports")}),
                 QStringLiteral("crash port: set"));
        QCOMPARE(crashPort({QStringLiteral("check-ports")}), QStringLiteral("crash port: none"));
#endif
    }

    // macOS: that port went to every child too (task-level ports are
    // inherited across fork and exec), and nothing reads it: a git, a
    // hook, ssh or a terminal's program that died of a signal waited
    // there for a reply, unkillable, until GitBolt quit, and a commit
    // waited out its five minutes. A test session's children start with
    // the system's crash handling, however GitBolt starts them.
    void testSessionChildrenKeepTheSystemsCrashHandling() {
#if !defined(GITBOLT_CRASH_HELPER) || !defined(Q_OS_MACOS)
        QSKIP("macOS only: the port is a Mach one");
#else
        QTemporaryDir home;
        QVERIFY(home.isValid());
        QProcess helper;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("HOME"), home.path());
        helper.setProcessEnvironment(env);
        helper.start(QStringLiteral(GITBOLT_CRASH_HELPER),
                     {QStringLiteral("test-session"), QStringLiteral("check-child-ports")});
        QVERIFY(helper.waitForFinished(120000));
        QCOMPARE(helper.exitStatus(), QProcess::NormalExit);
        QCOMPARE(helper.exitCode(), 0);
        QCOMPARE(QString::fromUtf8(helper.readAllStandardOutput()).trimmed(),
                 QStringLiteral("crash port: set\n"
                                "fork: none\n"
                                "qprocess: crash port: none\n"
                                "qprocess with a modifier: crash port: none\n"
                                "login shell: none"));
#endif
    }
};

QTEST_GUILESS_MAIN(TestCrashLogParser)
#include "TestCrashLogParser.moc"
