#pragma once

#include <QByteArray>
#include <QString>

namespace gitbolt::util {

/// Installs signal handlers for SIGSEGV, SIGABRT, SIGBUS, SIGFPE to write
/// a crash report to ~/.gitbolt/crash.log before terminating; a test
/// session writes ~/.gitbolt/crash-test.log instead, which nothing reads
/// or deletes, so a test's crash never replaces the user's report.
///
/// Call install() once at application startup.
/// On the next launch, call hasPendingCrashReport() to check for a log file.
///
/// The report (format 2) is a header, then the signal, then one line
/// per stack frame, newest first:
///
///   #<n> <module> +0x<offset> <symbol>+0x<offset>
///   #<n> <module> +0x<offset> ?           (no symbol)
///   #<n> ? 0x<address>                    (no module)
///
/// <module> is the image's file name only, with blanks made '_' (the
/// executable's real file name, however it was launched), and the
/// first offset is the frame's address relative to that image.
/// Shipped builds are stripped, so their own symbols are unreliable or
/// missing; image-relative offsets survive ASLR and let a maintainer
/// symbolize offline. Frames #0 and #1 are the handler and the
/// kernel's signal trampoline on POSIX. On macOS, #2 is the
/// interrupted PC from the signal context: backtrace() walks frame
/// pointers and never reaches the function that was running.
/// telemetry::CrashLogParser reads this format; keep the two in step.
class CrashHandler {
public:
    /// Install signal handlers. The header pre-rendered here records
    /// what the next launch needs to decide whether the report may be
    /// uploaded (CrashLogParser refuses all but official + none +
    /// user): the version that crashed, whether endpoints were
    /// compiled in (`officialBuild`), whether the environment opted
    /// out (`envOptOut`), and whether this is a test session
    /// (`testSession`). main.cpp passes them: it is the only file that
    /// sees the endpoints, and nothing else is set up this early.
    /// On macOS a test session's crash also stays out of the system's
    /// crash reporter (no "quit unexpectedly" box, no .ips report):
    /// its task-level EXC_CRASH port is one of its own. Its children
    /// (git, hooks, a terminal's shell) keep the system's handling.
    static void install(const char* appVersion, bool officialBuild,
                        bool envOptOut, bool testSession);

    /// The header install() writes: "Format: 2", the values above,
    /// then the Qt, OS, kernel and CPU lines. Takes the values instead
    /// of reading the environment, so a test renders the same header
    /// under ctest's and CI's environments as anywhere else.
    static QByteArray renderHeader(const char* appVersion, bool officialBuild,
                                   bool envOptOut, bool testSession);

    /// Returns true if a crash log from a previous session exists.
    static bool hasPendingCrashReport();

    /// Read and return the crash log contents.
    static QString readCrashReport();

    /// Remove the crash log file (after the user has been notified).
    static void clearCrashReport();

    /// Path to the crash log file.
    static QString crashLogPath();
    /// Where a test session's handler writes instead (install()'s
    /// `testSession`). The app never shows, sends or deletes it; the
    /// next test crash overwrites it.
    static QString testCrashLogPath();

private:
    /// Windows. POSIX installs an SA_SIGINFO handler of its own
    /// (CrashHandler.cpp), for the interrupted context.
    static void signalHandler(int signal);
    static void writeStackTrace(int signal);
};

} // namespace gitbolt::util
