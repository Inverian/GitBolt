#include "update/Package.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QObject>
#include <QStandardPaths>

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

} // namespace

PackageInfo detectPackage(const PackageProbe& probe) {
    const QString appDir = QDir::cleanPath(probe.appDirPath);
    if (!probe.appDirPath.isEmpty()) {
        // AppImage first: its runtime says so outright.
        if (!probe.appImage.isEmpty() && QFileInfo(probe.appImage).isFile()
            && appDir.endsWith(QLatin1String("/usr/bin"))) {
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
            && QFileInfo(bin.path() + QStringLiteral("/Uninstall.exe")).isFile())
            return {PackageKind::WindowsInstall, bin.path(), true, {}};

        if (probe.appImage.isEmpty() && appDir.endsWith(QLatin1String("/lib/gitbolt"))
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
#if defined(Q_OS_LINUX)
    probe.pkexec = QStandardPaths::findExecutable(QStringLiteral("pkexec"));
#endif
    return probe;
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
