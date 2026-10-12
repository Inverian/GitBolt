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

} // namespace

int main(int argc, char* argv[]) {
#ifdef Q_OS_MACOS
    keepCrashReporterOut();
#endif
    bool testSession = false;
    bool fault = false;
    for (int i = 1; i < argc; ++i) {
        testSession = testSession || std::strcmp(argv[i], "test-session") == 0;
        fault = fault || std::strcmp(argv[i], "fault") == 0;
    }
    gitbolt::util::CrashHandler::install("1.2.3", /*officialBuild=*/true,
                                         /*envOptOut=*/false, testSession);
    if (fault) {
        printFaultNowOffset();
        faultSoon();
    }
    crashSoon();
    return 0;
}
