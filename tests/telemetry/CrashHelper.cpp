//
// gitbolt_crash_helper — the death tests' crashing process
// (TestCrashLogParser::realCrashLogParses and
// testSessionCrashKeepsUsersLog). Installs the real crash handler, the
// way main() does but with the values injected, and dies on SIGSEGV.
// The test runs it with HOME pointed at a temporary directory and
// parses the crash.log the handler wrote there.
//
// Arguments, in any order:
//   test-session  install as a test session (the report goes to
//                 crash-test.log, stamped "Session: test")
//   fault         die of a real fault in a leaf function instead of
//                 raise(), after printing that function's offset in
//                 the executable ("faultNow +0x<hex>") to stdout
//   check-ports   (macOS) don't crash: print whether a task-level port
//                 takes EXC_CRASH ("crash port: set" or "crash port:
//                 none") once the handler is installed, and exit 0
//   check-child-ports
//                 (macOS) as check-ports, then the same for children
//                 started the ways GitBolt starts them: a fork ("fork:
//                 set|none"), a QProcess with and without a child
//                 modifier ("qprocess: …", "qprocess with a modifier:
//                 …"), and util::queryLoginShellPath()'s posix_spawn
//                 ("login shell: …"); and exit 0
//   report-ports  a child of check-child-ports: print "crash port: set"
//                 or "crash port: none", installing nothing
//   -ilc …        the same, as a login shell queryLoginShellPath() asks
//                 for its PATH: the marker, then "/crash-port-set" or
//                 "/crash-port-none"
//
// A user session's crash keeps macOS's crash reporter out of it here,
// as the helper's own doing (keepCrashReporterOut). A test session's
// leaves that to CrashHandler::install(), which does it for the app.
//
// Built as "gitbolt" (in a directory of its own) so the parser counts
// its frames as GitBolt's, as it would the app's.
//

#include "util/CrashHandler.h"
#include "util/LoginPath.h"

#include <QCoreApplication>
#include <QProcess>
#include <QtGlobal>

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <dlfcn.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef Q_OS_MACOS
#include <mach/mach.h>
#endif

namespace {

#ifdef Q_OS_MACOS
// Keeps macOS's crash reporter out of it. With a task-level port for
// EXC_CRASH and corpse notifications, the kernel hands those to this
// dying task instead of ReportCrash, so a test run leaves no report in
// the user's ~/Library/Logs/DiagnosticReports. The process still dies
// of SIGSEGV, after the handler under test has run.
void keepCrashReporterOut() {
    mach_port_t port = MACH_PORT_NULL;
    if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port) != KERN_SUCCESS
        || mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND)
               != KERN_SUCCESS)
        return;
    task_set_exception_ports(mach_task_self(), EXC_MASK_CRASH | EXC_MASK_CORPSE_NOTIFY, port,
                             EXCEPTION_DEFAULT, THREAD_STATE_NONE);
}
#endif

// A couple of frames of our own between main and the crash.
Q_NEVER_INLINE void crashNow() {
    std::raise(SIGSEGV);
}

Q_NEVER_INLINE void crashSoon() {
    crashNow();
}

// Volatile both ways, so no optimizer proves it null and turns the
// store into a trap instruction (SIGILL/SIGTRAP, which aren't caught).
volatile int* volatile g_nowhere = nullptr;

// A leaf: it calls nothing, so on arm64 it pushes no frame record, and
// a frame-pointer walk from the signal frame starts at its caller.
Q_NEVER_INLINE void faultNow() {
    *g_nowhere = 42;
}

Q_NEVER_INLINE void faultSoon() {
    faultNow();
}

// faultNow's start, relative to the executable's load address: what
// the crash.log's offsets are relative to.
void printFaultNowOffset() {
    const auto start = reinterpret_cast<std::uintptr_t>(&faultNow);
    Dl_info info{};
    if (::dladdr(reinterpret_cast<const void*>(start), &info) != 0 && info.dli_fbase) {
        std::printf("faultNow +0x%llx\n",
                    static_cast<unsigned long long>(
                        start - reinterpret_cast<std::uintptr_t>(info.dli_fbase)));
    }
    std::fflush(stdout);
}

#ifdef Q_OS_MACOS
// Whether a task-level port takes EXC_CRASH.
bool crashPortSet() {
    exception_mask_t masks[EXC_TYPES_COUNT];
    mach_msg_type_number_t count = EXC_TYPES_COUNT;
    exception_handler_t handlers[EXC_TYPES_COUNT];
    exception_behavior_t behaviors[EXC_TYPES_COUNT];
    thread_state_flavor_t flavors[EXC_TYPES_COUNT];
    if (task_get_exception_ports(mach_task_self(), EXC_MASK_CRASH, masks, &count, handlers,
                                 behaviors, flavors) != KERN_SUCCESS)
        return false;
    for (mach_msg_type_number_t i = 0; i < count; ++i) {
        if ((masks[i] & EXC_MASK_CRASH) && MACH_PORT_VALID(handlers[i]))
            return true;
    }
    return false;
}
#endif

#ifdef Q_OS_MACOS
const char* setOrNone(bool set) {
    return set ? "set" : "none";
}

// A child's word on its own port, run the ways GitBolt starts children.
void reportChildPorts(int argc, char* argv[]) {
    std::fflush(stdout);
    const pid_t pid = ::fork();
    if (pid == 0)
        ::_exit(crashPortSet() ? 1 : 0);
    int status = 0;
    const bool waited = pid > 0 && ::waitpid(pid, &status, 0) == pid && WIFEXITED(status);
    std::printf("fork: %s\n", waited ? setOrNone(WEXITSTATUS(status) == 1) : "<failed>");

    QCoreApplication app(argc, argv);
    const QString self = QCoreApplication::applicationFilePath();
    for (const bool modifier : {false, true}) {
        QProcess child;
        if (modifier)
            child.setChildProcessModifier([] { ::setsid(); });   // as GitProcess's
        child.start(self, {QStringLiteral("report-ports")});
        const bool done = child.waitForFinished(30000) && child.exitCode() == 0;
        std::printf("qprocess%s: %s\n", modifier ? " with a modifier" : "",
                    done ? child.readAllStandardOutput().trimmed().constData() : "<failed>");
    }
    const QString path = gitbolt::util::queryLoginShellPath(self, 30000);
    std::printf("login shell: %s\n",
                path == QLatin1String("/crash-port-set")    ? "set"
                : path == QLatin1String("/crash-port-none") ? "none"
                                                            : "<failed>");
    std::fflush(stdout);
}
#endif

} // namespace

int main(int argc, char* argv[]) {
#ifdef Q_OS_MACOS
    // A child of check-child-ports: its port as it started, nothing set.
    if (argc >= 2 && std::strcmp(argv[1], "-ilc") == 0) {
        std::printf("__GITBOLT_LOGIN_PATH__\n/crash-port-%s\n", setOrNone(crashPortSet()));
        return 0;
    }
    if (argc >= 2 && std::strcmp(argv[1], "report-ports") == 0) {
        std::printf("crash port: %s\n", setOrNone(crashPortSet()));
        return 0;
    }
#endif
    bool testSession = false;
    bool fault = false;
    bool checkPorts = false;
    bool checkChildPorts = false;
    for (int i = 1; i < argc; ++i) {
        testSession = testSession || std::strcmp(argv[i], "test-session") == 0;
        fault = fault || std::strcmp(argv[i], "fault") == 0;
        checkChildPorts = checkChildPorts || std::strcmp(argv[i], "check-child-ports") == 0;
        checkPorts = checkPorts || checkChildPorts || std::strcmp(argv[i], "check-ports") == 0;
    }
#ifdef Q_OS_MACOS
    if (!testSession && !checkPorts)
        keepCrashReporterOut();
#endif
    gitbolt::util::CrashHandler::install("1.2.3", /*officialBuild=*/true,
                                         /*envOptOut=*/false, testSession);
    if (checkPorts) {
#ifdef Q_OS_MACOS
        std::printf("crash port: %s\n", crashPortSet() ? "set" : "none");
        if (checkChildPorts)
            reportChildPorts(argc, argv);
#else
        std::printf("crash port: n/a\n");
#endif
        std::fflush(stdout);
        return 0;
    }
    if (fault) {
        printFaultNowOffset();
        faultSoon();
    }
    crashSoon();
    return 0;
}
