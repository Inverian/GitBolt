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
//   - the build ID: the "Build ID:" line round trip, the GlitchTip tag,
//     and the running executable's own ID in each platform's form
//   - Windows reports from the exception's context ("Stack:
//     exception"): every frame kept, the exception code read, an
//     uncaught C++ exception's raise frames dropped, the module list
//     ignored
//   - death tests: a helper installs the real handler and dies. POSIX:
//     the crash.log it wrote parses, with the handler and trampoline
//     frames gone, GitBolt's frames keep their module when it was
//     started through a link of another name, a fault's newest frame
//     is the faulting function itself, on the main thread or a worker,
//     and a home folder with non-ASCII letters works. Windows: faults
//     on any thread, a call through a null pointer, an uncaught C++
//     exception, abort() on a worker, an invalid CRT parameter, and a
//     report folder with non-ASCII letters. A test session's crash
//     leaves the user's crash.log alone, and on macOS stays out of the
//     system's crash reporter
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

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#endif

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

QJsonObject eventTags(const CrashEvent& event) {
    return gitbolt::telemetry::glitchTipCrash(event).value(QStringLiteral("tags")).toObject();
}

QString eventValue(const CrashEvent& event) {
    return gitbolt::telemetry::glitchTipCrash(event)
        .value(QStringLiteral("exception")).toObject()
        .value(QStringLiteral("values")).toArray().first().toObject()
        .value(QStringLiteral("value")).toString();
}

// A Windows report from the exception's context, with `code` as its
// "Exception:" line and a module list after the frames.
QString contextReport(const QString& code, const QStringList& frames,
                      const QString& signal = QStringLiteral("SIGSEGV (11)")) {
    return QString::fromUtf8(withCrashOs(officialHeader(), "windows 11"))
           + QStringLiteral("Signal: %1\nException: %2\nStack: exception\nEpoch: 1760000000\n\n"
                            "--- Stack Trace ---\n").arg(signal, code)
           + frames.join(QLatin1Char('\n'))
           + QStringLiteral("\n\n--- Modules ---\n"
                            "GitBolt.exe base=0x7ff6a1230000 size=0x1c8000 time=0x6730a1b2\n"
                            "\n=== End of Report ===\n");
}

// The helper's environment: HOME `home`, and its report folder there
// on Windows, where HOME doesn't move the profile folder.
QProcessEnvironment helperEnvironment(const QString& home) {
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("HOME"), home);
#ifdef Q_OS_WIN
    env.insert(QStringLiteral("GITBOLT_CRASH_HELPER_DIR"),
               QDir::toNativeSeparators(home + QStringLiteral("/.gitbolt")));
#endif
    return env;
}

// "<function> +0x<hex>" as the helper prints it before crashing.
std::optional<quint64> printedOffset(const QByteArray& out, const QString& function) {
    const QRegularExpressionMatch m =
        QRegularExpression(QStringLiteral("^%1 \\+0x([0-9a-f]+)\\r?$").arg(function),
                           QRegularExpression::MultilineOption)
            .match(QString::fromUtf8(out));
    if (!m.hasMatch())
        return std::nullopt;
    return m.captured(1).toULongLong(nullptr, 16);
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
        QTest::addColumn<QString>("buildId");
        QTest::addColumn<QString>("exceptionCode");
        // 0.9.x reports: no Build ID, no Exception line.
        QTest::newRow("macOS") << "macos.log" << "SIGSEGV" << 11 << "macOS" << "15.6" << "arm64"
                               << qint64(1760000000) << 14 << "GitBolt" << quint64(0x5c3f0)
                               << "start" << QString() << QString();
        QTest::newRow("Linux") << "linux.log" << "SIGABRT" << 6 << "Linux" << "ubuntu 24.04"
                               << "x86_64" << qint64(1760000123) << 12 << "libc.so.6"
                               << quint64(0x9eb2c) << "__libc_start_call_main" << QString()
                               << QString();
        // #0 the handler, #1-#5 the CRT and the loader dispatching the
        // exception, up to KiUserExceptionDispatcher.
        QTest::newRow("Windows") << "windows.log" << "SIGSEGV" << 11 << "Windows" << "11"
                                 << "x86_64" << qint64(1760000456) << 6 << "GitBolt.exe"
                                 << quint64(0x51a40) << "RtlUserThreadStart" << QString()
                                 << QString();
        // An unhandled C++ exception: #0 the handler, #1-#3 abort() from
        // terminate(), #4 the CRT's unhandled-exception filter (inside
        // GitBolt.exe), #5-#9 the dispatch, #10-#12 the throw itself.
        QTest::newRow("Windows C++ exception")
            << "windows-exception.log" << "SIGABRT" << 22 << "Windows" << "11" << "x86_64"
            << qint64(1760000789) << 5 << "GitBolt.exe" << quint64(0x51a40)
            << "RtlUserThreadStart" << QString() << QString();
        // From the exception's context: #0 is the crash point, and no
        // frame goes. No names (positions only), and the module list
        // after the frames is never read.
        QTest::newRow("Windows, from the context")
            << "windows-context.log" << "SIGSEGV" << 11 << "Windows" << "11" << "x86_64"
            << qint64(1760000999) << 8 << "GitBolt.exe" << quint64(0x51a40) << QString()
            << "3f2a9c1d5e6b47a8b9c0d1e2f3a4b5c61" << "0xc0000005";
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
        QFETCH(QString, buildId);
        QFETCH(QString, exceptionCode);

        const std::optional<CrashEvent> event = CrashLogParser::parse(fixture(name));
        QVERIFY(event);
        QCOMPARE(event->buildId, buildId);
        QCOMPARE(event->exceptionCode, exceptionCode);
        QJsonObject tags{{QStringLiteral("kind"), QStringLiteral("crash")}};
        if (!buildId.isEmpty())
            tags.insert(QStringLiteral("build_id"), buildId);
        QCOMPARE(eventTags(*event), tags);
        QCOMPARE(eventValue(*event),
                 exceptionCode.isEmpty() ? signal : signal + QLatin1Char(' ') + exceptionCode);
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
                                     "Secret", "AppRun", "foohook", "ibus", "base=", "pdb",
                                     "7ff6a1230000"})
            QVERIFY2(!json.contains(sentinel), sentinel);
    }

    // The Build ID line: last in the header, lower-cased by the parser,
    // and the GlitchTip tag build_id. A malformed one keeps the report.
    void buildIdRoundTrip_data() {
        QTest::addColumn<QByteArray>("id");
        QTest::addColumn<QString>("expected");
        QTest::newRow("GNU build ID (sha1)")
            << QByteArray("0123456789abcdef0123456789abcdef01234567")
            << "0123456789abcdef0123456789abcdef01234567";
        QTest::newRow("Mach-O UUID, upper case")
            << QByteArray("EC89B33079773318A1942396EEF8118B") << "ec89b33079773318a1942396eef8118b";
        QTest::newRow("PDB GUID and age")
            << QByteArray("3f2a9c1d5e6b47a8b9c0d1e2f3a4b5c61") << "3f2a9c1d5e6b47a8b9c0d1e2f3a4b5c61";
        QTest::newRow("not hex") << QByteArray("xyz") << QString();
        QTest::newRow("15 digits") << QByteArray("0123456789abcde") << QString();
        QTest::newRow("65 digits") << QByteArray(65, 'a') << QString();
        QTest::newRow("a path") << QByteArray("C:/Users/alice") << QString();
    }

    void buildIdRoundTrip() {
        QFETCH(QByteArray, id);
        QFETCH(QString, expected);
        const QByteArray header = withCrashOs(
            CrashHandler::renderHeader("0.1.5", true, false, false, id), "macos 15.6");
        QVERIFY(header.endsWith("\nBuild ID: " + id + "\n"));
        QVERIFY(header.startsWith(withCrashOs(CrashHandler::renderHeader("0.1.5", true, false, false),
                                              "macos 15.6")));

        const std::optional<CrashEvent> event = CrashLogParser::parse(report(
            header, {QStringLiteral("#0 GitBolt +0x10 ?"),
                     QStringLiteral("#1 libsystem_platform.dylib +0x20 _sigtramp+0x8"),
                     QStringLiteral("#2 GitBolt +0x30 ?")}));
        QVERIFY(event);
        QCOMPARE(event->buildId, expected);
        QJsonObject tags{{QStringLiteral("kind"), QStringLiteral("crash")}};
        if (!expected.isEmpty())
            tags.insert(QStringLiteral("build_id"), expected);
        QCOMPARE(eventTags(*event), tags);
        // Not in the fingerprint: other binaries' offsets differ anyway.
        for (const QString& part : event->fingerprint)
            QVERIFY(expected.isEmpty() || !part.contains(expected));
    }

    // The running executable's ID, as the handler writes it: the Mach-O
    // UUID, the GNU build ID (sha1), or the PDB's GUID and age, which
    // dbghelp reads from the file on its own.
    void executableBuildId() {
        const QString id = QString::fromLatin1(CrashHandler::executableBuildId());
#if defined(Q_OS_MACOS)
        QVERIFY2(QRegularExpression(QStringLiteral("^[0-9a-f]{32}$")).match(id).hasMatch(),
                 qPrintable(id));
#elif defined(Q_OS_LINUX)
        QVERIFY2(QRegularExpression(QStringLiteral("^[0-9a-f]{40}$")).match(id).hasMatch(),
                 qPrintable(id));
#elif defined(Q_OS_WIN)
        SYMSRV_INDEX_INFOW info{};
        info.sizeofstruct = static_cast<DWORD>(sizeof(info));
        const std::wstring file =
            QDir::toNativeSeparators(QCoreApplication::applicationFilePath()).toStdWString();
        QVERIFY(SymSrvGetFileIndexInfoW(file.c_str(), &info, 0));
        QString expected = QStringLiteral("%1%2%3")
                               .arg(uint(info.guid.Data1), 8, 16, QLatin1Char('0'))
                               .arg(uint(info.guid.Data2), 4, 16, QLatin1Char('0'))
                               .arg(uint(info.guid.Data3), 4, 16, QLatin1Char('0'));
        for (const unsigned char byte : info.guid.Data4)
            expected += QStringLiteral("%1").arg(uint(byte), 2, 16, QLatin1Char('0'));
        expected += QString::number(uint(info.age), 16);
        QCOMPARE(id, expected);
#else
        QSKIP("no build ID on this platform");
#endif
    }

    // "Stack: exception" (Windows, from the exception's context): #0 is
    // where it happened, and every frame stays, a fault in a system
    // module's too. Anything else on the Exception line is ignored, and
    // never a reason to refuse the report.
    void windowsContextKeepsEveryFrame() {
        const std::optional<CrashEvent> event = CrashLogParser::parse(contextReport(
            QStringLiteral("0xc0000005"),
            {QStringLiteral("#0 ntdll.dll +0x1e4b8 ?"), QStringLiteral("#1 ucrtbase.dll +0x200 ?"),
             QStringLiteral("#2 GitBolt.exe +0x300 ?"), QStringLiteral("#3 GitBolt.exe +0x400 ?")}));
        QVERIFY(event);
        QCOMPARE(event->frames.size(), 4);
        QCOMPARE(event->frames.last().package, QStringLiteral("ntdll.dll"));
        QCOMPARE(event->frames.last().offset, std::optional<quint64>(0x1e4b8));
        QCOMPARE(event->exceptionCode, QStringLiteral("0xc0000005"));

        for (const QString& code : {QStringLiteral("C:\\Users\\alice"), QStringLiteral("0xC0000005"),
                                    QStringLiteral("0xc00000051"), QStringLiteral("c0000005")}) {
            const std::optional<CrashEvent> odd = CrashLogParser::parse(
                contextReport(code, {QStringLiteral("#0 GitBolt.exe +0x300 ?")}));
            QVERIFY2(odd, qPrintable(code));
            QVERIFY(odd->exceptionCode.isEmpty());
            QCOMPARE(eventValue(*odd), QStringLiteral("SIGSEGV"));
            QVERIFY(!eventJson(*odd).contains("alice"));
        }
    }

    // An uncaught C++ exception's context is RaiseException's, under the
    // CRT's throw: those frames go, and the crash is the thrower's, as
    // the 0.9.x rules have it for the same crash.
    void windowsContextCxxExceptionStartsAtTheThrower() {
        const std::optional<CrashEvent> event = CrashLogParser::parse(contextReport(
            QStringLiteral("0xe06d7363"),
            {QStringLiteral("#0 KERNELBASE.dll +0x5fb4c ?"),
             QStringLiteral("#1 VCRUNTIME140.dll +0x6720 ?"),
             QStringLiteral("#2 GitBolt.exe +0x51a40 ?"), QStringLiteral("#3 GitBolt.exe +0x52b10 ?"),
             QStringLiteral("#4 Qt6Core.dll +0x2b1c0 ?")},
            QStringLiteral("SIGABRT (22)")));
        QVERIFY(event);
        QCOMPARE(event->frames.size(), 3);
        QCOMPARE(event->frames.last().package, QStringLiteral("GitBolt.exe"));
        QCOMPARE(event->frames.last().offset, std::optional<quint64>(0x51a40));
        QCOMPARE(eventValue(*event), QStringLiteral("SIGABRT 0xe06d7363"));

        const std::optional<CrashEvent> old =
            CrashLogParser::parse(fixture(QStringLiteral("windows-exception.log")));
        QVERIFY(old);
        QCOMPARE(event->fingerprint, old->fingerprint);
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

    // Only a whole name is allowed: one that merely contains, starts or
    // ends with an allowed name (UIFoundation, a GitBolt plugin) is
    // <other>, with no position or function.
    void moduleNamesMatchWhole() {
        const QStringList others = {
            QStringLiteral("UIFoundation"),     QStringLiteral("libswiftFoundation.dylib"),
            QStringLiteral("AcmeAppKitHelper.dylib"), QStringLiteral("GitBoltPlugin.dylib"),
            QStringLiteral("foo-gitbolt"),      QStringLiteral("Xdyld"),
            QStringLiteral("x_kernel32.dll"),   QStringLiteral("hook_ucrtbase.dll"),
            QStringLiteral("NTDLL.dll.bak")};
        const QStringList allowed = {
            QStringLiteral("GitBolt"),      QStringLiteral("gitbolt"),       QStringLiteral("AppKit"),
            QStringLiteral("Foundation"),   QStringLiteral("CoreFoundation"), QStringLiteral("HIToolbox"),
            QStringLiteral("dyld"),         QStringLiteral("libdyld.dylib"), QStringLiteral("ntdll.dll"),
            QStringLiteral("KERNELBASE.dll"), QStringLiteral("kernel32.dll"), QStringLiteral("ucrtbase.dll"),
            QStringLiteral("vcruntime140.dll"), QStringLiteral("msvcp140.dll")};
        // #0 and #1 are the handler's own, dropped by position.
        QStringList lines = {QStringLiteral("#0 GitBolt +0x10 ?"), QStringLiteral("#1 libsystem_platform.dylib +0x20 ?")};
        int index = 2;
        for (const QString& name : others + allowed) {
            lines << QStringLiteral("#%1 %2 +0x40 secret_hook+0x4").arg(index).arg(name);
            ++index;
        }
        const std::optional<CrashEvent> event = CrashLogParser::parse(report(officialHeader(), lines));
        QVERIFY(event);
        QStringList packages;
        for (const CrashFrame& frame : event->frames) {
            packages.prepend(frame.package);   // newest last
            if (frame.package == QLatin1String("<other>")) {
                QVERIFY(!frame.offset);
                QVERIFY(frame.function.isEmpty());
                QVERIFY(!frame.inApp);
            }
        }
        QStringList expected(others.size(), QStringLiteral("<other>"));
        expected += allowed;
        QCOMPARE(packages, expected);
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

    // ucrtbased.dll: a Debug build's CRT (a local Windows run's).
    void windowsAbortDropsCrtFrames() {
        for (const QString& crt : {QStringLiteral("ucrtbase.dll"), QStringLiteral("ucrtbased.dll")}) {
            const std::optional<CrashEvent> event = CrashLogParser::parse(report(
                withCrashOs(officialHeader(), "windows 11"),
                {QStringLiteral("#0 GitBolt.exe +0x100 ?"), QStringLiteral("#1 %1 +0x200 raise+0x1").arg(crt),
                 QStringLiteral("#2 %1 +0x300 abort+0x1").arg(crt), QStringLiteral("#3 GitBolt.exe +0x400 ?"),
                 QStringLiteral("#4 KERNEL32.DLL +0x500 BaseThreadInitThunk+0x1")},
                QStringLiteral("SIGABRT (22)")));
            QVERIFY(event);
            QCOMPARE(event->os, QStringLiteral("Windows"));
            QCOMPARE(event->frames.size(), 2);
            QCOMPARE(event->frames.last().package, QStringLiteral("GitBolt.exe"));
            QCOMPARE(event->frames.last().offset, std::optional<quint64>(0x400));
        }
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
    // on SIGSEGV with HOME in a temporary directory (POSIX).
    void realCrashLogParses_data() {
        QTest::addColumn<QString>("launchedAs");
        QTest::addColumn<QStringList>("args");
        QTest::addColumn<QString>("homeName");   // the HOME folder's name
        QTest::newRow("by its own name") << QString() << QStringList() << QStringLiteral("home");
        // glibc's dladdr names the main program by argv[0]: started
        // through a link, by the link's name.
        QTest::newRow("through a link of another name")
            << QStringLiteral("gitbolt-launcher") << QStringList() << QStringLiteral("home");
        // A real fault, in a leaf function of GitBolt's own: that
        // function must be the newest frame. macOS's backtrace() walks
        // frame pointers and never has the interrupted PC, so the
        // caller used to pass for the crash point.
        QTest::newRow("a fault in a leaf function")
            << QString() << QStringList{QStringLiteral("fault")} << QStringLiteral("home");
        QTest::newRow("on a worker thread")
            << QString() << QStringList{QStringLiteral("fault"), QStringLiteral("worker")}
            << QStringLiteral("home");
        QTest::newRow("a non-ASCII home")
            << QString() << QStringList() << QStringLiteral("Jörg Ж 王");
    }

    void realCrashLogParses() {
#if !defined(GITBOLT_CRASH_HELPER) || defined(Q_OS_WIN)
        QSKIP("POSIX: windowsRealCrashLogParses covers Windows' handler");
#else
        QFETCH(QString, launchedAs);
        QFETCH(QStringList, args);
        QFETCH(QString, homeName);
        const bool fault = args.contains(QStringLiteral("fault"));
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString home = tmp.filePath(homeName);
        QVERIFY(QDir().mkpath(home));
        QString program = QStringLiteral(GITBOLT_CRASH_HELPER);
        if (!launchedAs.isEmpty()) {
            const QString link = tmp.filePath(launchedAs);
            QVERIFY(QFile::link(program, link));
            program = link;
        }
        QProcess helper;
        helper.setProcessEnvironment(helperEnvironment(home));
        helper.start(program, args);
        QVERIFY(helper.waitForFinished(60000));
        QCOMPARE(helper.exitStatus(), QProcess::CrashExit);

        QFile file(home + QStringLiteral("/.gitbolt/crash.log"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString log = QString::fromUtf8(file.readAll());
        QVERIFY2(log.startsWith(QString::fromUtf8(CrashHandler::renderHeader("0.1.0", true, false, false))),
                 qPrintable(log));
        QVERIFY2(log.contains(QStringLiteral("\nSignal: SIGSEGV (11)\n")), qPrintable(log));
        // The helper's own ID, in the platform's form.
        const QRegularExpressionMatch id =
            QRegularExpression(QStringLiteral("\nBuild ID: ([0-9a-f]+)\n")).match(log);
        QVERIFY2(id.hasMatch(), qPrintable(log));
#if defined(Q_OS_MACOS)
        QCOMPARE(id.captured(1).size(), 32);
#elif defined(Q_OS_LINUX)
        QCOMPARE(id.captured(1).size(), 40);
#endif

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
        QCOMPARE(event->buildId, id.captured(1));
        QCOMPARE(event->frames.size(), lines.size() - 2);
        // The newest frame left is #2, where raise() was running, or
        // the faulting instruction.
        const CrashFrame& top = event->frames.last();
        if (top.package != QLatin1String("<other>")) {
            QCOMPARE(top.package, modules.at(2));
            QCOMPARE(top.offset, std::optional<quint64>(offsets.at(2)));
        }
        if (fault) {
            const std::optional<quint64> faultNow =
                printedOffset(helper.readAllStandardOutput(), QStringLiteral("faultNow"));
            QVERIFY(faultNow);
            QVERIFY2(top.inApp && top.offset && *top.offset >= *faultNow
                         && *top.offset < *faultNow + 0x100,
                     qPrintable(log));
        }
        // crashNow, crash and main, as the app's own: no names.
        int inApp = 0;
        for (const CrashFrame& frame : event->frames) {
            if (!frame.inApp)
                continue;
            ++inApp;
            QCOMPARE(frame.function, QStringLiteral("<unknown>"));
            QVERIFY(frame.offset != std::optional<quint64>(offsets.at(0)));   // not the handler
        }
        QVERIFY2(inApp >= 1, qPrintable(log));
        QVERIFY(!eventJson(*event).contains(home.toUtf8()));
#endif
    }

    // Windows' handler, for real: an unhandled-exception filter on every
    // thread (std::signal(SIGSEGV) is per thread in the UCRT, so a
    // worker's crash used to leave no report), SIGABRT, the CRT's
    // invalid-parameter handler, and a report folder whose name the
    // ANSI code page can't spell.
    void windowsRealCrashLogParses_data() {
        QTest::addColumn<QStringList>("args");
        QTest::addColumn<QString>("signal");
        QTest::addColumn<QString>("exception");   // the Exception line; empty: none
        QTest::addColumn<bool>("crashExit");
        QTest::addColumn<quint32>("exitCode");
        QTest::addColumn<bool>("fromContext");    // "Stack: exception"
        QTest::addColumn<QString>("function");    // where the newest frame is
        QTest::addColumn<QString>("folder");      // the report folder's parent's name
        QTest::newRow("main-thread fault")
            << QStringList{QStringLiteral("fault")} << "SIGSEGV (11)" << "0xc0000005" << true
            << quint32(0xC0000005) << true << "faultNow" << "home";
        QTest::newRow("worker-thread fault")
            << QStringList{QStringLiteral("fault"), QStringLiteral("worker")} << "SIGSEGV (11)"
            << "0xc0000005" << true << quint32(0xC0000005) << true << "faultNow" << "home";
        // #0 is "? 0x0"; the frame before it is the caller.
        QTest::newRow("call through a null pointer")
            << QStringList{QStringLiteral("null-call")} << "SIGSEGV (11)" << "0xc0000005" << true
            << quint32(0xC0000005) << true << "callNull" << "home";
        // QProcess calls exit codes past 0xCFFFFFFF normal exits.
        QTest::newRow("uncaught C++ exception")
            << QStringList{QStringLiteral("throw")} << "SIGABRT (22)" << "0xe06d7363" << false
            << quint32(0xE06D7363) << true << "throwNow" << "home";
        QTest::newRow("abort on a worker")
            << QStringList{QStringLiteral("abort"), QStringLiteral("worker")} << "SIGABRT (22)"
            << QString() << false << quint32(3) << false << "abortNow" << "home";
        QTest::newRow("invalid CRT parameter")
            << QStringList{QStringLiteral("invalid-parameter")} << "SIGABRT (22)" << "0xc0000417"
            << true << quint32(0xC0000417) << false << "invalidParameterNow" << "home";
        QTest::newRow("non-ASCII report folder")
            << QStringList{QStringLiteral("fault")} << "SIGSEGV (11)" << "0xc0000005" << true
            << quint32(0xC0000005) << true << "faultNow" << QStringLiteral("Jörg Ж 王");
    }

    void windowsRealCrashLogParses() {
#if !defined(GITBOLT_CRASH_HELPER) || !defined(Q_OS_WIN)
        QSKIP("Windows only: realCrashLogParses covers POSIX");
#else
        QFETCH(QStringList, args);
        QFETCH(QString, signal);
        QFETCH(QString, exception);
        QFETCH(bool, crashExit);
        QFETCH(quint32, exitCode);
        QFETCH(bool, fromContext);
        QFETCH(QString, function);
        QFETCH(QString, folder);
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString home = tmp.filePath(folder);
        QProcess helper;
        helper.setProcessEnvironment(helperEnvironment(home));
        helper.start(QStringLiteral(GITBOLT_CRASH_HELPER), args);
        QVERIFY(helper.waitForFinished(60000));
        const QByteArray out = helper.readAllStandardOutput();
        QCOMPARE(helper.exitStatus(), crashExit ? QProcess::CrashExit : QProcess::NormalExit);
        QCOMPARE(quint32(helper.exitCode()), exitCode);

        QFile file(home + QStringLiteral("/.gitbolt/crash.log"));
        QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(file.fileName()));
        const QString log = QString::fromUtf8(file.readAll());
        QVERIFY2(log.startsWith(QString::fromUtf8(CrashHandler::renderHeader("0.1.0", true, false, false))),
                 qPrintable(log));
        QVERIFY2(log.contains(QStringLiteral("\nSignal: %1\n").arg(signal)), qPrintable(log));
        QCOMPARE(log.contains(QStringLiteral("\nException: ")), !exception.isEmpty());
        if (!exception.isEmpty())
            QVERIFY2(log.contains(QStringLiteral("\nException: %1\n").arg(exception)), qPrintable(log));
        QCOMPARE(log.contains(QStringLiteral("\nStack: exception\n")), fromContext);
        // Linked with /DEBUG: the PDB's GUID and age.
        const QRegularExpressionMatch id =
            QRegularExpression(QStringLiteral("\nBuild ID: ([0-9a-f]{33,40})\n")).match(log);
        QVERIFY2(id.hasMatch(), qPrintable(log));

        // Frame lines, numbered from 0, then the module list: GitBolt's
        // first, with its size, time stamp and the same id.
        static const QRegularExpression line(QStringLiteral(
            "^#(\\d+) (?:(\\S+) \\+0x([0-9a-f]+) \\?|\\? 0x[0-9a-f]+)$"));
        const QString stackMarker = QStringLiteral("--- Stack Trace ---\n");
        const QString modulesMarker = QStringLiteral("\n\n--- Modules ---\n");
        const qsizetype begin = log.indexOf(stackMarker);
        const qsizetype middle = log.indexOf(modulesMarker);
        const qsizetype end = log.indexOf(QStringLiteral("\n\n=== End of Report ==="));
        QVERIFY2(begin > 0 && middle > begin && end > middle, qPrintable(log));
        const qsizetype framesAt = begin + stackMarker.size();
        const QStringList lines = log.mid(framesAt, middle - framesAt).split(QLatin1Char('\n'));
        QVERIFY2(lines.size() >= 3, qPrintable(log));
        for (qsizetype i = 0; i < lines.size(); ++i) {
            const QRegularExpressionMatch m = line.match(lines.at(i));
            QVERIFY2(m.hasMatch(), qPrintable(lines.at(i)));
            QCOMPARE(m.captured(1).toInt(), int(i));
        }
        const qsizetype modulesAt = middle + modulesMarker.size();
        const QStringList moduleLines = log.mid(modulesAt, end - modulesAt).split(QLatin1Char('\n'));
        QVERIFY2(QRegularExpression(QStringLiteral(
                     "^gitbolt\\.exe base=0x[0-9a-f]+ size=0x[0-9a-f]+ time=0x[0-9a-f]{8} "
                     "pdb=\\S+\\.pdb id=%1$").arg(id.captured(1)))
                     .match(moduleLines.first()).hasMatch(),
                 qPrintable(log));
        for (const QString& module : moduleLines)
            QVERIFY2(!module.startsWith(QLatin1Char('#')) && module.contains(QStringLiteral(" size=0x")),
                     qPrintable(module));

        const std::optional<CrashEvent> event = CrashLogParser::parse(log);
        QVERIFY2(event, qPrintable(log));
        QCOMPARE(event->signal + QStringLiteral(" (%1)").arg(event->signalNumber), signal);
        QCOMPARE(event->exceptionCode, exception);
        QCOMPARE(event->buildId, id.captured(1));
        QVERIFY(event->frames.size() >= 2);

        const std::optional<quint64> start = printedOffset(out, function);
        QVERIFY2(start, out.constData());
        // A call through null: the newest frame is "? 0x0", the next
        // one the caller's return address.
        const bool nullCall = args.contains(QStringLiteral("null-call"));
        if (nullCall)
            QCOMPARE(event->frames.last().package, QStringLiteral("<other>"));
        const CrashFrame& top = event->frames.at(event->frames.size() - (nullCall ? 2 : 1));
        QVERIFY2(top.inApp && top.offset && *top.offset >= *start && *top.offset < *start + 0x100,
                 qPrintable(log));
        QVERIFY(!eventJson(*event).contains("base="));
#endif
    }

    // A test session's crash goes to crash-test.log. crash.log is the
    // user's (a bridge run outside the e2e harness keeps the real
    // HOME), and the handler truncates the file it writes: a report of
    // theirs still waiting for their next launch used to be replaced by
    // one that launch then refused to send.
    void testSessionCrashKeepsUsersLog() {
#ifndef GITBOLT_CRASH_HELPER
        QSKIP("no crash helper in this build");
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
        helper.setProcessEnvironment(helperEnvironment(home.path()));
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
