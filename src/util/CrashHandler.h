#pragma once

#include <QString>

namespace gitbolt::util {

/// Installs signal handlers for SIGSEGV, SIGABRT, SIGBUS, SIGFPE to write
/// a crash report to ~/.gitbolt/crash.log before terminating.
///
/// Call install() once at application startup.
/// On the next launch, call hasPendingCrashReport() to check for a log file.
class CrashHandler {
public:
    /// Install signal handlers.
    static void install();

    /// Returns true if a crash log from a previous session exists.
    static bool hasPendingCrashReport();

    /// Read and return the crash log contents.
    static QString readCrashReport();

    /// Remove the crash log file (after the user has been notified).
    static void clearCrashReport();

    /// Path to the crash log file.
    static QString crashLogPath();

private:
    static void signalHandler(int signal);
    static void writeStackTrace(int signal);
};

} // namespace gitbolt::util
