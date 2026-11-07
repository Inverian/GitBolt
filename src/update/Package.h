#pragma once

#include "update/Version.h"

#include <QString>

#include <functional>

namespace gitbolt::update {

/// How the running copy of GitBolt was installed, which decides what
/// the updater downloads and how it puts it in place. Each kind is one
/// of the release's packages; anything else (a build tree, a
/// `cmake --install` prefix, a hand-extracted AppImage) is Unknown and
/// is never replaced: the updater would be overwriting something it
/// didn't install.
enum class PackageKind { Unknown, MacBundle, WindowsInstall, Deb, AppImage };

struct PackageInfo {
    PackageKind kind = PackageKind::Unknown;
    /// What an update replaces. MacBundle: the bundle (…/GitBolt.app);
    /// WindowsInstall: the install folder, the parent of bin; Deb: the
    /// package's folder, /usr/lib/gitbolt; AppImage: the .AppImage file
    /// itself. Empty for Unknown.
    QString target;
    bool canInstall = false;
    /// One user-facing sentence or two when !canInstall: why, and what
    /// to do instead. Plain text.
    QString whyNot;
};

/// What detectPackage looks at, gathered by currentProbe() from the
/// running process. A struct, so a test lays out folders like each
/// package and decides what is writable, on any OS.
struct PackageProbe {
    /// QCoreApplication::applicationDirPath(): the folder of the binary.
    QString appDirPath;
    /// $APPIMAGE, which the AppImage runtime sets to the .AppImage
    /// file's absolute path. Empty when unset.
    QString appImage;
    /// Where dpkg lists the files of each installed package.
    QString dpkgInfoDir = QStringLiteral("/var/lib/dpkg/info");
    /// pkexec's absolute path, found on PATH, or empty.
    QString pkexec;
    /// Whether a path may be written; null means QFileInfo::isWritable.
    std::function<bool(const QString&)> writable;
};

/// The package the probe describes, by the rules below, from paths
/// alone (no #ifdef), so every kind is testable on every OS:
///
///   - MacBundle: appDirPath is X.app/Contents/MacOS and
///     X.app/Contents/Frameworks exists (macdeployqt put Qt there; a
///     build tree's bundle has none). Installable unless it runs from
///     a disk image (/Volumes/) or from App Translocation's read-only
///     copy, and only if both the bundle and its folder are writable:
///     the swap renames the bundle within its folder.
///   - WindowsInstall: appDirPath is …\bin and ..\Uninstall.exe exists
///     (the NSIS installer's). Always installable: the installer asks
///     for elevation itself (UAC).
///   - Deb: appDirPath ends in /lib/gitbolt, dpkg lists the package
///     (gitbolt.list), and $APPIMAGE is unset. Installable when pkexec
///     is there to ask for root; otherwise whyNot gives the command.
///   - AppImage: $APPIMAGE names an existing file and appDirPath ends
///     in /usr/bin (inside the mounted image). Installable when the
///     AppImage's folder is writable: the new file is written next to
///     it and renamed over it.
///   - Unknown: anything else.
PackageInfo detectPackage(const PackageProbe& probe);

/// The running process's probe: applicationDirPath, $APPIMAGE, and
/// pkexec on PATH (Linux only).
PackageProbe currentProbe();

/// The release asset each kind installs from, as CI names them:
/// GitBolt-<v>-Darwin-arm64.dmg, GitBolt-<v>-Windows-AMD64.exe,
/// gitbolt_<v>_amd64.deb, GitBolt-<v>-x86_64.AppImage. Empty for
/// Unknown. The updater checks a download's name against this, and
/// CI's release job asserts the same names.
QString assetNameFor(PackageKind kind, const Version& version);

} // namespace gitbolt::update
