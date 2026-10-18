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
/// follows: on macOS .command, .tool and .terminal (Terminal runs
/// them) and .fileloc, .inetloc and .webloc (they open what they point
/// to); on Windows whatever PATHEXT lists, and .lnk, .url, .scr, .pif,
/// .msi, .js, .jse, .vbs, .vbe, .wsf and .hta; elsewhere .desktop. A
/// folder only when it is a bundle (see folderToShow()). The type is
/// read from `file`'s own name, so resolve a symbolic link first.
bool mayRunWhenOpened(const QFileInfo& file);

/// The folder to hand the desktop to show `folder`: `folder` itself,
/// unless macOS opens it as a bundle. A folder named Tool.app is an
/// app to Finder, and opening it launches the app it holds, so the
/// nearest folder above that isn't a bundle is shown instead, with
/// the bundle in it. Elsewhere `folder` as it is.
QString folderToShow(const QString& folder);

} // namespace gitbolt::util
