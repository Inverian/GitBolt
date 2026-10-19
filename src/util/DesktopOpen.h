#pragma once

#include <QByteArray>
#include <QString>

#include <cstdint>

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
/// runs them), .jar, .class and .jnlp (Java runs them), .py and its
/// kin (Python Launcher runs them), and the location files, .fileloc,
/// .webloc, .url and the like (they open what they point to); on
/// Windows whatever PATHEXT lists, the shortcuts, installers, scripts,
/// control panels and the like whose default verb runs them (.lnk,
/// .url, .msi, .cpl, .chm, .hta, .application...), and the scripts an
/// installed interpreter runs (.sh, .py, .rb, .pl...); elsewhere
/// .desktop, .jar, .jnlp, and the Windows programs Wine runs. A folder
/// only when it is a bundle (see folderToShow()). The type is read
/// from `file`'s own name, so resolve a symbolic link first.
///
/// Where the work tree keeps no modes (`modesKept` false: core.fileMode
/// is off, as git sets it at clone or init where a file's mode doesn't
/// stick, on FAT, exFAT and NTFS drives and WSL's /mnt/c), every file
/// is marked executable, and the mark says nothing of the file. There
/// a marked file may run if the index has it executable (`indexMode`
/// 0100755; 0 where the index has none), or if the platform's opener
/// runs it for the mark all the same: on macOS one with no extension
/// (Launch Services makes a Unix executable of it, which Terminal
/// runs), a .dylib (one to Launch Services too), or a shell or Perl
/// script (a terminal app such as iTerm2 can take them, and runs them),
/// elsewhere a program (see isProgram()).
bool mayRunWhenOpened(const QFileInfo& file, bool modesKept = true, uint32_t indexMode = 0);

/// What mayRunWhenOpened() judges a file by, the folder aside.
struct FileToOpen {
    QString name;            ///< its name, which gives its type
    bool executable = false; ///< the file system marks it executable
    bool modesKept = true;   ///< the work tree keeps modes (core.fileMode)
    uint32_t indexMode = 0;  ///< the index's mode for it, 0 where it has none
    QByteArray head;         ///< its first bytes, where they are needed
};

/// mayRunWhenOpened() of a file that isn't a folder, from what is
/// known of it.
bool mayRunWhenOpened(const FileToOpen& file);

/// Whether the openers that xdg-open hands a file to take it for a
/// program, and run it, or ask whether to, when it is marked
/// executable: an ELF binary, a file that starts with "#!" (LXDE's
/// pcmanfm asks about any), or one whose type the shared MIME database
/// they go by counts as a program's, by its name (a .sh file) or by
/// `head`, its first bytes. KDE runs a marked script where the app
/// that asked lets it, and opens one that isn't marked in an editor.
bool isProgram(const QString& name, const QByteArray& head);

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

/// Whether `path`, relative to a folder a repository names it in (a
/// submodule's path, in the work tree), stays in that folder as it is
/// written, on every platform: not empty, no root, drive or UNC host
/// ("/x", "C:/x", "//host/share", "\\host\share"), and no ".." step.
/// Judged from the text alone, so ask it before anything looks on
/// disk: on Windows a look at //host/share/x is a connection to that
/// host, the user's credentials offered, and one that doesn't answer
/// holds the window up until it times out.
bool isRelativeInside(const QString& path);

} // namespace gitbolt::util
