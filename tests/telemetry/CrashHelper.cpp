//
// gitbolt_crash_helper — the death tests' crashing process
// (TestCrashLogParser::realCrashLogParses, windowsRealCrashLogParses
// and testSessionCrashKeepsUsersLog, and tests/tools/test_symbolize.py).
// Installs the real crash handler, the way main() does but with the
// values injected, and dies. The test runs it with HOME pointed at a
// temporary directory and parses the crash.log the handler wrote there.
//
// GITBOLT_CRASH_HELPER_DIR: the report folder instead of ~/.gitbolt.
// Required on Windows, where HOME doesn't move the profile folder and
// a run would overwrite the developer's own crash.log.
//
// Arguments, in any order:
//   test-session  install as a test session (the report goes to
//                 crash-test.log, stamped "Session: test")
//   fault         die of a real fault in a leaf function instead of
//                 raise() (on Windows, which has no raise() handler,
//                 the default)
//   null-call     call through a null function pointer
//   throw         throw a std::runtime_error nothing catches
//   abort         call std::abort()
//   invalid-parameter
//                 fclose(nullptr): the CRT's invalid-parameter handler
//                 (Windows; elsewhere a fault)
//   worker        do the above on a std::thread, not the main thread
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
// Before it crashes, it prints the crashing function's start relative
// to the executable ("<function> +0x<hex>"), which the crash.log's
// offsets are relative to too.
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
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <thread>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

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

// Where `function` starts, relative to the executable's load address:
// what the crash.log's offsets are relative to.
void printOffset(const char* name, const void* function) {
    const auto start = reinterpret_cast<std::uintptr_t>(function);
    std::uintptr_t base = 0;
#ifdef Q_OS_WIN
    HMODULE module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                               | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(function), &module))
        base = reinterpret_cast<std::uintptr_t>(module);
#else
    Dl_info info{};
    if (::dladdr(function, &info) != 0 && info.dli_fbase)
        base = reinterpret_cast<std::uintptr_t>(info.dli_fbase);
#endif
    if (base != 0)
        std::printf("%s +0x%llx\n", name, static_cast<unsigned long long>(start - base));
    std::fflush(stdout);
}

// Volatile both ways, so no optimizer proves them null and turns the
// access into a trap instruction (SIGILL/SIGTRAP, which aren't caught).
volatile int* volatile g_nowhere = nullptr;
using Function = void (*)();
volatile Function g_nullFunction = nullptr;
#ifdef Q_OS_WIN
std::FILE* volatile g_noFile = nullptr;
#endif
volatile int g_after = 0;

// A couple of frames of our own between main and the crash.
Q_NEVER_INLINE void crashNow() {
    std::raise(SIGSEGV);
}

// A leaf: it calls nothing, so on arm64 it pushes no frame record, and
// a frame-pointer walk from the signal frame starts at its caller.
Q_NEVER_INLINE void faultNow() {
    *g_nowhere = 42;
}

// The store after the call keeps it a call (not a jump), so this
// function is the frame the null address returns to.
Q_NEVER_INLINE void callNull() {
    g_nullFunction();
    g_after = 1;
}

Q_NEVER_INLINE void throwNow() {
    throw std::runtime_error("C:\\Users\\alice\\secret-repo");
}

Q_NEVER_INLINE void abortNow() {
    std::abort();
}

#ifdef Q_OS_WIN
Q_NEVER_INLINE void invalidParameterNow() {
    const int result = std::fclose(g_noFile);
    std::printf("fclose: %d\n", result);
}
#endif

enum class Crash { Raise, Fault, NullCall, Throw, Abort, InvalidParameter };

Q_NEVER_INLINE void crash(Crash how) {
    switch (how) {
    case Crash::Raise:
        crashNow();
        break;
    case Crash::Fault:
        printOffset("faultNow", reinterpret_cast<const void*>(&faultNow));
        faultNow();
        break;
    case Crash::NullCall:
        printOffset("callNull", reinterpret_cast<const void*>(&callNull));
        callNull();
        break;
    case Crash::Throw:
        printOffset("throwNow", reinterpret_cast<const void*>(&throwNow));
        throwNow();
        break;
    case Crash::Abort:
        printOffset("abortNow", reinterpret_cast<const void*>(&abortNow));
        abortNow();
        break;
    case Crash::InvalidParameter:
#ifdef Q_OS_WIN
        printOffset("invalidParameterNow", reinterpret_cast<const void*>(&invalidParameterNow));
        invalidParameterNow();
#else
        printOffset("faultNow", reinterpret_cast<const void*>(&faultNow));
        faultNow();
#endif
        break;
    }
    g_after = 2;
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
    bool onWorker = false;
    bool checkPorts = false;
    bool checkChildPorts = false;
#ifdef Q_OS_WIN
    Crash how = Crash::Fault;   // raise(SIGSEGV) reaches no handler there
#else
    Crash how = Crash::Raise;
#endif
    for (int i = 1; i < argc; ++i) {
        const auto is = [&](const char* word) { return std::strcmp(argv[i], word) == 0; };
        testSession = testSession || is("test-session");
        onWorker = onWorker || is("worker");
        checkChildPorts = checkChildPorts || is("check-child-ports");
        checkPorts = checkPorts || checkChildPorts || is("check-ports");
        if (is("fault"))
            how = Crash::Fault;
        else if (is("null-call"))
            how = Crash::NullCall;
        else if (is("throw"))
            how = Crash::Throw;
        else if (is("abort"))
            how = Crash::Abort;
        else if (is("invalid-parameter"))
            how = Crash::InvalidParameter;
    }
    const QString reportDir = qEnvironmentVariable("GITBOLT_CRASH_HELPER_DIR");
#ifdef Q_OS_WIN
    if (reportDir.isEmpty() && !checkPorts) {
        std::fprintf(stderr, "GITBOLT_CRASH_HELPER_DIR is not set\n");
        return 2;
    }
    // No Windows Error Reporting box on a desktop, should the handler
    // under test let a crash through.
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
#endif
#ifdef Q_OS_MACOS
    if (!testSession && !checkPorts)
        keepCrashReporterOut();
#endif
    gitbolt::util::CrashHandler::install("0.1.0", /*officialBuild=*/true,
                                         /*envOptOut=*/false, testSession, reportDir);
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
    if (onWorker) {
        std::thread worker([how] { crash(how); });
        worker.join();
    } else {
        crash(how);
    }
    return 0;
}
