#pragma once

#include <QString>

class QFileInfo;

namespace gitbolt::util {

// What GitBolt checks before it hands a path in a work tree to the
// desktop (QDesktopServices::openUrl). The desktop opens a file with
// its default app, and for some files that app runs it. What a
// repository checks out is the repository's choice: git keeps a
// file's executable bit, and sets no quarantine (macOS) or Mark of
// the Web (Windows), so no Gatekeeper or SmartScreen prompt stands in
// the way either. On macOS an executable file with no extension, such
// as a CHANGELOG at mode 755, opens in Terminal, which runs it.

/// Whether the desktop may run `file` (or what it points to) when
/// asked to open it, rather than show it. True for a file marked
/// executable, and for the types the platform's opener runs or
/// follows: on macOS .command, .tool, .term and .terminal (Terminal
/// runs them), .jar, .class and .jnlp (Java runs them), and the
/// location files, .fileloc, .webloc, .url and the like (they open
/// what they point to); on Windows whatever PATHEXT lists, and the
/// shortcuts, installers, scripts, control panels and the like whose
/// default verb runs them (.lnk, .url, .msi, .cpl, .chm, .hta,
/// .application...); elsewhere .desktop, .jar, and the Windows
/// programs Wine runs. A folder only when it is a bundle (see
/// folderToShow()). The type is read from `file`'s own name, so
/// resolve a symbolic link first.
bool mayRunWhenOpened(const QFileInfo& file);

/// The folder to hand the desktop to show `path`, and only ever a
/// folder: the desktop opens a file it is handed, and runs some (see
/// mayRunWhenOpened()). `path` is resolved to its real path first,
/// since what a symbolic link leads to is what the desktop opens. If
/// that isn't a folder, the folder it is in. On macOS a folder named
/// Tool.app is an app to Finder, and opening it launches the app it
/// holds, so the nearest folder above that isn't a bundle is shown
/// instead, with the bundle in it. A path that doesn't exist shows the
/// nearest folder above it that does.
QString folderToShow(const QString& path);

/// Whether `path` is `folder` or in it, once both are resolved to
/// their real paths: a symbolic link (or a folder on the way that is
/// one) is judged by where it leads. False if either doesn't exist.
bool isWithinFolder(const QString& path, const QString& folder);

} // namespace gitbolt::util
