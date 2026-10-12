#pragma once

#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>
#include <QStringView>

#include <optional>

namespace gitbolt::telemetry {

/// One stack frame, reduced to what may leave the machine.
struct CrashFrame {
    /// The module's file name when it is on the allowlist (GitBolt's
    /// own binary, Qt, libgit2, the C/C++ runtimes and a few system
    /// libraries), else "<other>": an injected third-party module
    /// (input method, accessibility tool) would tell what else the
    /// user has installed.
    QString package;
    /// GitBolt's frames: always "<unknown>". Shipped binaries are
    /// stripped, so any name there is the nearest surviving symbol,
    /// which is wrong; the offset is what a maintainer symbolizes.
    /// Other allowlisted modules: the demangled name without its
    /// parameter list, when it passes a strict character check (for
    /// stripped system libraries it may be approximate, which in_app
    /// false leaves to the reader). "<other>" and failed checks: empty.
    QString function;
    /// Address relative to the module's load address (survives ASLR);
    /// nullopt for "<other>".
    std::optional<quint64> offset;
    bool inApp = false;
};

/// A crash.log reduced for GlitchTip. Every field comes from the
/// report itself, never from the running app: a crash just before an
/// upgrade is filed under the version that crashed.
struct CrashEvent {
    QString release;         ///< "App version:", e.g. "0.9.5"
    QDateTime at;            ///< "Epoch:", UTC
    QString signal;          ///< SIGSEGV, SIGABRT, SIGBUS, SIGFPE or UNKNOWN
    int signalNumber = 0;
    QString os;              ///< "macOS", "Windows" or "Linux"
    QString osVersion;       ///< empty when the report's didn't pass the check
    QString arch;
    QString qtVersion;       ///< the runtime version
    QList<CrashFrame> frames;   ///< oldest first, handler frames dropped, at most 64
    /// "crash", the signal, then "gitbolt@<release>:" and 16 hex
    /// digits of a hash over the first three GitBolt frames' offsets,
    /// counted from the crash point ("none" without such frames):
    /// stable for one build, opaque without its binary, never free
    /// text, and free of '/' like everything else in the event.
    QStringList fingerprint;
};

/// Reads a report written by util::CrashHandler (format and frame
/// lines in CrashHandler.h).
class CrashLogParser {
public:
    /// nullopt unless the header says "Format: 2", "Build: official",
    /// "Opt-out: none" and "Session: user", and the version, signal
    /// and epoch are well-formed. Every build and session under a HOME
    /// shares ~/.gitbolt/crash.log, so a crash from a source build, an
    /// opted-out session or a test run would otherwise be uploaded by
    /// the user's next official launch; format-1 reports (0.9.4 and
    /// older) don't say where they came from at all.
    static std::optional<CrashEvent> parse(QStringView log);
};

} // namespace gitbolt::telemetry
