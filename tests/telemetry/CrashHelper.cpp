//
// gitbolt_crash_helper — the death test's crashing process
// (TestCrashLogParser::realCrashLogParses). Installs the real crash
// handler, the way main() does but with the values injected, and dies
// on SIGSEGV. The test runs it with HOME pointed at a temporary
// directory and parses the crash.log the handler wrote there.
//
// Built as "gitbolt" (in a directory of its own) so the parser counts
// its frames as GitBolt's, as it would the app's.
//

#include "util/CrashHandler.h"

#include <QtGlobal>

#include <csignal>

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

} // namespace

int main() {
#ifdef Q_OS_MACOS
    keepCrashReporterOut();
#endif
    gitbolt::util::CrashHandler::install("1.2.3", /*officialBuild=*/true,
                                         /*envOptOut=*/false, /*testSession=*/false);
    crashSoon();
    return 0;
}
