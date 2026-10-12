//
// Crash report headers for the telemetry tests' synthetic reports.
//
// CrashHandler::renderHeader writes the host's "OS:" line, and
// CrashLogParser picks its handler-frame rules by that line: a Windows
// report drops the CRT and loader frames, a POSIX one the signal
// trampoline. A synthetic POSIX report has to say which OS it comes
// from, or the same test expects other frames on the Windows runner.
//

#pragma once

#include <QByteArray>
#include <QByteArrayView>

#include "util/CrashHandler.h"

namespace gitbolt::test {

/// `header` with its "OS: …" line made "OS: <os>".
inline QByteArray withCrashOs(QByteArray header, QByteArrayView os) {
    const qsizetype begin = header.indexOf("\nOS: ");
    const qsizetype end = begin < 0 ? -1 : header.indexOf('\n', begin + 1);
    if (end < 0)
        return header;
    return header.replace(begin + 5, end - begin - 5, os);
}

/// CrashHandler::renderHeader's header as a macOS host writes it,
/// whatever the host running the test.
inline QByteArray crashHeader(const char* appVersion, bool officialBuild, bool envOptOut,
                              bool testSession) {
    return withCrashOs(gitbolt::util::CrashHandler::renderHeader(appVersion, officialBuild,
                                                                 envOptOut, testSession),
                       "macos 15.6");
}

} // namespace gitbolt::test
