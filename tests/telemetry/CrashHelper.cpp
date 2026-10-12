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
//
// A user session's crash keeps macOS's crash reporter out of it here,
// as the helper's own doing (keepCrashReporterOut). A test session's
// leaves that to CrashHandler::install(), which does it for the app.
//
// Built as "gitbolt" (in a directory of its own) so the parser counts
// its frames as GitBolt's, as it would the app's.
//

#include "util/CrashHandler.h"

#include <QtGlobal>

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <dlfcn.h>

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

} // namespace

int main(int argc, char* argv[]) {
    bool testSession = false;
    bool fault = false;
    bool checkPorts = false;
    for (int i = 1; i < argc; ++i) {
        testSession = testSession || std::strcmp(argv[i], "test-session") == 0;
        fault = fault || std::strcmp(argv[i], "fault") == 0;
        checkPorts = checkPorts || std::strcmp(argv[i], "check-ports") == 0;
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
