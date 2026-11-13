#pragma once

#include "update/Version.h"

#include <QString>
#include <QStringList>

#include <functional>

namespace gitbolt::update {

/// How the running copy of GitBolt was installed, which decides what
/// the updater downloads and how it puts it in place. Each kind is one
/// of the release's packages; anything else (a build tree, a
/// `cmake --install` prefix, someone else's package) is Unknown and is
/// never replaced: the updater would be overwriting something it didn't
/// install.
enum class PackageKind { Unknown, MacBundle, WindowsInstall, Deb };

struct PackageInfo {
    PackageKind kind = PackageKind::Unknown;
    /// What an update replaces. MacBundle: the bundle (…/GitBolt.app);
    /// WindowsInstall: the install folder, the parent of bin; Deb: the
    /// package's folder, /usr/lib/gitbolt. Empty for Unknown.
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
    /// Where dpkg lists the files of each installed package.
    QString dpkgInfoDir = QStringLiteral("/var/lib/dpkg/info");
    /// pkexec's absolute path, found on PATH, or empty.
    QString pkexec;
    /// Windows: the UninstallString values (as stored, quotes and all)
    /// of HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\GitBolt
    /// in the 32- and 64-bit registry views (the 32-bit installer writes
    /// the 32-bit one, WOW6432Node; cmake/NsisScript.cmake names the key):
    /// where the installed GitBolt is, which is the one the NSIS installer
    /// uninstalls before it installs. Empty when there is none.
    QStringList registeredUninstallers;
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
///   - WindowsInstall: appDirPath is …\bin, and ..\Uninstall.exe (the
///     NSIS installer's) exists or the registry names it, missing: the
///     update's installer puts it back. Installable (UAC is asked to let
///     it run, while GitBolt is still open) unless the registry names
///     another install: a copied install folder isn't the registered one,
///     and the installer would uninstall that one and move it here.
///   - Deb: appDirPath ends in /lib/gitbolt and dpkg lists the package
///     (gitbolt.list). Installable when pkexec is there to ask for
///     root; otherwise whyNot gives the command.
///   - Unknown: anything else.
PackageInfo detectPackage(const PackageProbe& probe);

/// The running process's probe: applicationDirPath, pkexec on PATH
/// (Linux only), and the registered uninstallers (Windows only).
PackageProbe currentProbe();

/// Windows: PackageProbe::registeredUninstallers, read from the
/// registry, which this only reads: it never creates a key, even with
/// administrator rights. `key` is under HKEY_LOCAL_MACHINE, in both
/// views (a test names one of its own). Empty elsewhere.
QStringList registeredWindowsUninstallers(
    const QString& key = QStringLiteral("SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\GitBolt"));

/// The release asset each kind installs from, as CI names them:
/// GitBolt-<v>-Darwin-arm64.dmg, GitBolt-<v>-Windows-AMD64.exe,
/// gitbolt_<v>_amd64.deb. Empty for Unknown. The updater checks a download's name against this, and
/// CI's release job asserts the same names.
QString assetNameFor(PackageKind kind, const Version& version);

} // namespace gitbolt::update
