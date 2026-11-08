#include "update/Package.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QObject>
#include <QStandardPaths>

#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>  // RegOpenKeyExW, RegGetValueW

#include <string>
#include <vector>
#endif

namespace gitbolt::update {

namespace {

bool writable(const PackageProbe& probe, const QString& path) {
    return probe.writable ? probe.writable(path) : QFileInfo(path).isWritable();
}

QString cannotChange(const QString& folder) {
    return QObject::tr("You don't have permission to change %1, so GitBolt can't replace itself there. "
                       "Download the new version from the release page and install it, or ask an "
                       "administrator to.")
        .arg(QDir::toNativeSeparators(folder));
}

// …/X.app/Contents/MacOS → …/X.app, or empty.
QString bundleOf(const QString& appDir) {
    const QFileInfo macos(appDir);
    if (macos.fileName() != QLatin1String("MacOS"))
        return {};
    const QFileInfo contents(macos.path());
    if (contents.fileName() != QLatin1String("Contents"))
        return {};
    const QString bundle = contents.path();
    const QString name = QFileInfo(bundle).fileName();
    if (name.size() <= 4 || !name.endsWith(QLatin1String(".app"), Qt::CaseInsensitive))
        return {};
    return bundle;
}

PackageInfo macBundle(const PackageProbe& probe, const QString& bundle) {
    PackageInfo info{PackageKind::MacBundle, bundle, false, {}};
    // Both spellings: the path GitBolt was started by, and where it
    // leads (Finder's translocated copy, a /Volumes link).
    const QString canonical = QFileInfo(bundle).canonicalFilePath();
    const auto under = [&](QLatin1String marker, bool prefix) {
        for (const QString& path : {bundle, canonical}) {
            if (prefix ? path.startsWith(marker) : path.contains(marker))
                return true;
        }
        return false;
    };
    if (under(QLatin1String("/Volumes/"), true)) {
        info.whyNot = QObject::tr("GitBolt is running from a disk image or another volume, which it doesn't "
                                  "update. Drag GitBolt into your Applications folder, open it from there, and "
                                  "check for updates again.");
    } else if (under(QLatin1String("/AppTranslocation/"), false)) {
        info.whyNot = QObject::tr("macOS is running GitBolt from a temporary read-only copy, because it wasn't "
                                  "moved after it was downloaded. Quit GitBolt, move it into your Applications "
                                  "folder with Finder, open it again, and check for updates again.");
    } else {
        const QString folder = QFileInfo(bundle).path();
        if (!writable(probe, folder) || !writable(probe, bundle))
            info.whyNot = cannotChange(folder);
        else
            info.canInstall = true;
    }
    return info;
}

// The binary runs from inside the AppImage $APPDIR names: its folder is
// $APPDIR/usr/bin, compared canonically (applicationDirPath comes from
// /proc/self/exe, resolved; the runtime's mount or extraction may be
// under a TMPDIR reached through a link).
bool runsFromAppDir(const QString& appDirPath, const QString& appDirEnv) {
    if (appDirEnv.isEmpty())
        return false;
    const QString running = QFileInfo(appDirPath).canonicalFilePath();
    return !running.isEmpty() && running == QFileInfo(appDirEnv + QStringLiteral("/usr/bin")).canonicalFilePath();
}

// A Windows path from the registry or the probe, comparable: forward
// slashes, no quotes around it, no arguments after a quoted one.
QString windowsComparable(QString path) {
    path = path.trimmed();
    if (path.startsWith(u'"')) {
        const qsizetype close = path.indexOf(u'"', 1);
        path = close > 0 ? path.mid(1, close - 1) : path.mid(1);
    }
    path.replace(u'\\', u'/');
    return QDir::cleanPath(path);
}

bool sameWindowsPath(const QString& a, const QString& b) {
    const QString ca = windowsComparable(a);
    const QString cb = windowsComparable(b);
    if (ca.compare(cb, Qt::CaseInsensitive) == 0)
        return true;
    // 8.3 names, junctions: where both exist, where they lead.
    const QString ra = QFileInfo(ca).canonicalFilePath();
    return !ra.isEmpty() && ra.compare(QFileInfo(cb).canonicalFilePath(), Qt::CaseInsensitive) == 0;
}

} // namespace

PackageInfo detectPackage(const PackageProbe& probe) {
    const QString appDir = QDir::cleanPath(probe.appDirPath);
    if (!probe.appDirPath.isEmpty()) {
        // AppImage first: its runtime says so outright, if this binary
        // is inside the image it names.
        if (!probe.appImage.isEmpty() && QFileInfo(probe.appImage).isFile()
            && runsFromAppDir(appDir, probe.appDir)) {
            const QFileInfo image(probe.appImage);
            PackageInfo info{PackageKind::AppImage, image.absoluteFilePath(), false, {}};
            if (writable(probe, image.absolutePath()))
                info.canInstall = true;
            else
                info.whyNot = cannotChange(image.absolutePath());
            return info;
        }

        if (const QString bundle = bundleOf(appDir);
            !bundle.isEmpty() && QFileInfo(bundle + QStringLiteral("/Contents/Frameworks")).isDir())
            return macBundle(probe, bundle);

        const QFileInfo bin(appDir);
        if (bin.fileName().compare(QLatin1String("bin"), Qt::CaseInsensitive) == 0
            && QFileInfo(bin.path() + QStringLiteral("/Uninstall.exe")).isFile()) {
            // The installer's .onInit runs the registered uninstaller
            // (CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL), whichever
            // folder it is in: from a copy of the install folder, an
            // update would remove the real install and move it here.
            // With nothing registered there is nothing to uninstall.
            PackageInfo info{PackageKind::WindowsInstall, bin.path(), true, {}};
            const QString own = bin.path() + QStringLiteral("/Uninstall.exe");
            for (const QString& registered : probe.registeredUninstallers) {
                if (windowsComparable(registered).isEmpty() || sameWindowsPath(registered, own))
                    continue;
                QString folder = windowsComparable(registered);
                folder = folder.left(qMax<qsizetype>(0, folder.lastIndexOf(u'/')));
                info.canInstall = false;
                info.whyNot = QObject::tr("This copy of GitBolt isn't the installed one, which is in %1, so it "
                                          "can't update itself: the installer would update that one instead. "
                                          "Update from the installed GitBolt, or install the new version with "
                                          "the installer from the release page.")
                                  .arg(QString(folder).replace(u'/', u'\\'));
                break;
            }
            return info;
        }

        if (appDir.endsWith(QLatin1String("/lib/gitbolt"))
            && QFileInfo(QDir(probe.dpkgInfoDir).filePath(QStringLiteral("gitbolt.list"))).isFile()) {
            PackageInfo info{PackageKind::Deb, appDir, false, {}};
            if (!probe.pkexec.isEmpty())
                info.canInstall = true;
            else
                info.whyNot = QObject::tr("Installing updates needs pkexec, which isn't installed. Download the "
                                          "new .deb from the release page and install it from a terminal: "
                                          "sudo apt install ./gitbolt_<version>_amd64.deb");
            return info;
        }
    }
    return {PackageKind::Unknown, {}, false,
            QObject::tr("This copy of GitBolt wasn't installed from a GitBolt release package, so it can't "
                        "update itself. Download the new version from the release page.")};
}

PackageProbe currentProbe() {
    PackageProbe probe;
    probe.appDirPath = QCoreApplication::applicationDirPath();
    probe.appImage = qEnvironmentVariable("APPIMAGE");
    probe.appDir = qEnvironmentVariable("APPDIR");
#if defined(Q_OS_LINUX)
    probe.pkexec = QStandardPaths::findExecutable(QStringLiteral("pkexec"));
#endif
    probe.registeredUninstallers = registeredWindowsUninstallers();
    return probe;
}

QStringList registeredWindowsUninstallers(const QString& key) {
    QStringList found;
#if defined(Q_OS_WIN)
    // Read only, with the registry's own calls: RegOpenKeyExW opens a
    // key that is there and creates nothing. QSettings did create: one
    // made from a path opens its key for writing, and makes it when it
    // is missing and the process may (Qt 6's qsettings_win.cpp,
    // createOrOpenKey). So every GitBolt with administrator rights (UAC
    // off, the built-in Administrator, Run as administrator) left an
    // empty ...\Uninstall\GitBolt in both views, which the 32-bit NSIS
    // uninstaller never removes. That is where CI's empty 64-bit key
    // came from: test_update_package, run as an administrator on the
    // hosted runner, read this before the NSIS smoke test installed
    // anything.
    // The 32-bit NSIS stub writes under WOW6432Node (the 32-bit view);
    // the 64-bit view too, in case a later installer does.
    const std::wstring path = key.toStdWString();
    for (const REGSAM view : {REGSAM(KEY_WOW64_32KEY), REGSAM(KEY_WOW64_64KEY)}) {
        HKEY opened = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ | view, &opened) != ERROR_SUCCESS)
            continue;
        // A string, REG_SZ, which RegGetValueW always ends with a null;
        // read again, bigger, if it didn't fit (ERROR_MORE_DATA gives
        // the size), as when it grew meanwhile.
        std::vector<wchar_t> buffer(512);
        for (int attempt = 0; attempt < 3; ++attempt) {
            DWORD bytes = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
            const LSTATUS status = RegGetValueW(opened, nullptr, L"UninstallString", RRF_RT_REG_SZ, nullptr,
                                                buffer.data(), &bytes);
            if (status == ERROR_MORE_DATA) {
                buffer.resize(bytes / sizeof(wchar_t) + 1);
                continue;
            }
            if (status == ERROR_SUCCESS) {
                const QString value = QString::fromWCharArray(buffer.data()).trimmed();
                if (!value.isEmpty() && !found.contains(value))
                    found << value;
            }
            break;
        }
        RegCloseKey(opened);
    }
#else
    Q_UNUSED(key)
#endif
    return found;
}

QString assetNameFor(PackageKind kind, const Version& version) {
    const QString v = version.toString();
    switch (kind) {
    case PackageKind::MacBundle:
        return QStringLiteral("GitBolt-%1-Darwin-arm64.dmg").arg(v);
    case PackageKind::WindowsInstall:
        return QStringLiteral("GitBolt-%1-Windows-AMD64.exe").arg(v);
    case PackageKind::Deb:
        return QStringLiteral("gitbolt_%1_amd64.deb").arg(v);
    case PackageKind::AppImage:
        return QStringLiteral("GitBolt-%1-x86_64.AppImage").arg(v);
    case PackageKind::Unknown:
        break;
    }
    return {};
}

} // namespace gitbolt::update
