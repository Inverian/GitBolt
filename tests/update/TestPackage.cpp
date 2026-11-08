//
// TestPackage — what kind of install the running copy is, and whether
// the updater may replace it (update/Package.h). Every kind is laid out
// in a temporary folder and judged from paths alone, so all of them
// run on every OS:
//
//   - a macOS bundle with Contents/Frameworks (the release's), not one
//     without (a build tree's); refused in App Translocation's
//     read-only copy, or where the bundle or its folder can't be
//     written (on a disk image too: TestInstaller mounts one);
//   - a Windows install: bin\ beside the NSIS uninstaller; not refused
//     for a folder the user can't write (the installer asks for
//     elevation), only when the registry's uninstaller (either view)
//     is another folder's: a copy of the install;
//   - a .deb: /usr/lib/gitbolt that dpkg lists, even with another
//     AppImage's $APPIMAGE inherited; refused without pkexec, and the
//     refusal gives the command;
//   - an AppImage: $APPIMAGE an existing file and the binary in
//     $APPDIR/usr/bin (canonically); refused where its folder can't be
//     written; a squashfs-root that inherited another AppImage's
//     $APPIMAGE and $APPDIR is Unknown;
//   - anything else (a build tree, a cmake --install prefix, a
//     hand-extracted AppImage) is Unknown, never installable;
//   - the release's asset name for each kind, and the probe of this
//     very test binary (a build tree: Unknown);
//   - Windows: reading the registered uninstallers creates no key, in
//     either registry view, even with administrator rights.
//

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include "FakeApps.h"
#include "update/Package.h"

#if defined(Q_OS_WIN)
#include <QDateTime>
#include <QScopeGuard>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>
#endif

using namespace gitbolt::update;
using gitbolt::test::writeFile;

Q_DECLARE_METATYPE(gitbolt::update::PackageKind)

namespace {

QString kindName(PackageKind kind) {
    switch (kind) {
    case PackageKind::Unknown: return QStringLiteral("Unknown");
    case PackageKind::MacBundle: return QStringLiteral("MacBundle");
    case PackageKind::WindowsInstall: return QStringLiteral("WindowsInstall");
    case PackageKind::Deb: return QStringLiteral("Deb");
    case PackageKind::AppImage: return QStringLiteral("AppImage");
    }
    return {};
}

} // namespace

class TestPackage : public QObject {
    Q_OBJECT

private:
    QTemporaryDir root_;

    QString path(const QString& relative) const { return QDir::cleanPath(root_.filePath(relative)); }

    // A probe of `appDir` (relative to the root) where everything can
    // be written but `readOnly`.
    PackageProbe probe(const QString& appDir, const QStringList& readOnly = {}) const {
        PackageProbe p;
        p.appDirPath = path(appDir);
        p.dpkgInfoDir = path(QStringLiteral("var/lib/dpkg/info"));
        QStringList blocked;
        for (const QString& r : readOnly)
            blocked << path(r);
        p.writable = [blocked](const QString& candidate) { return !blocked.contains(QDir::cleanPath(candidate)); };
        return p;
    }

    // An installed bundle: <folder>/GitBolt.app with its Frameworks.
    QString bundle(const QString& folder, bool frameworks = true) const {
        const QString app = path(folder + QStringLiteral("/GitBolt.app"));
        QDir().mkpath(app + QStringLiteral("/Contents/MacOS"));
        if (frameworks)
            QDir().mkpath(app + QStringLiteral("/Contents/Frameworks"));
        return app;
    }

private slots:
    void initTestCase() { QVERIFY(root_.isValid()); }

    void findsTheMacBundle() {
        const QString app = bundle(QStringLiteral("mac/Applications"));
        const PackageInfo info = detectPackage(probe(QStringLiteral("mac/Applications/GitBolt.app/Contents/MacOS")));
        QCOMPARE(kindName(info.kind), kindName(PackageKind::MacBundle));
        QCOMPARE(info.target, app);
        QVERIFY(info.canInstall);
        QVERIFY(info.whyNot.isEmpty());
        // Renamed by the user: still the bundle it is, under its name.
        const QString renamed = path(QStringLiteral("mac2/GitBolt Beta.app"));
        QDir().mkpath(renamed + QStringLiteral("/Contents/MacOS"));
        QDir().mkpath(renamed + QStringLiteral("/Contents/Frameworks"));
        const PackageInfo second = detectPackage(probe(QStringLiteral("mac2/GitBolt Beta.app/Contents/MacOS")));
        QCOMPARE(kindName(second.kind), kindName(PackageKind::MacBundle));
        QCOMPARE(second.target, renamed);
    }

    // A build tree's bundle has no Frameworks (Qt isn't deployed into
    // it), and the folder must really be X.app/Contents/MacOS.
    void aBuildTreeBundleIsUnknown() {
        bundle(QStringLiteral("build/src/app"), false);
        PackageInfo info = detectPackage(probe(QStringLiteral("build/src/app/GitBolt.app/Contents/MacOS")));
        QCOMPARE(kindName(info.kind), kindName(PackageKind::Unknown));
        QVERIFY(!info.canInstall);
        QVERIFY(info.whyNot.contains(QLatin1String("wasn't installed from a GitBolt release package")));

        QDir().mkpath(path(QStringLiteral("odd/GitBolt/Contents/MacOS")));
        QDir().mkpath(path(QStringLiteral("odd/GitBolt/Contents/Frameworks")));
        info = detectPackage(probe(QStringLiteral("odd/GitBolt/Contents/MacOS")));
        QCOMPARE(kindName(info.kind), kindName(PackageKind::Unknown));
        QDir().mkpath(path(QStringLiteral("odd/.app/Contents/MacOS")));
        QDir().mkpath(path(QStringLiteral("odd/.app/Contents/Frameworks")));
        info = detectPackage(probe(QStringLiteral("odd/.app/Contents/MacOS")));
        QCOMPARE(kindName(info.kind), kindName(PackageKind::Unknown));
    }

    // Writable is decided by the probe; both the bundle (renamed into
    // the Trash) and its folder (where the swap happens) must be.
    void refusesABundleItCantReplace_data() {
        QTest::addColumn<QString>("readOnly");
        QTest::newRow("its folder") << QStringLiteral("ro/Applications");
        QTest::newRow("the bundle") << QStringLiteral("ro/Applications/GitBolt.app");
    }

    void refusesABundleItCantReplace() {
        QFETCH(QString, readOnly);
        bundle(QStringLiteral("ro/Applications"));
        const PackageInfo info =
            detectPackage(probe(QStringLiteral("ro/Applications/GitBolt.app/Contents/MacOS"), {readOnly}));
        QCOMPARE(kindName(info.kind), kindName(PackageKind::MacBundle));
        QVERIFY(!info.canInstall);
        QVERIFY2(info.whyNot.contains(QLatin1String("permission")), qPrintable(info.whyNot));
        QVERIFY2(info.whyNot.contains(QDir::toNativeSeparators(path(QStringLiteral("ro/Applications")))),
                 qPrintable(info.whyNot));
    }

    // App Translocation's copy of a quarantined app that wasn't moved
    // is read-only, whatever the probe says. (A bundle on a disk image,
    // under /Volumes/, is refused too; TestInstaller mounts a real one
    // for that.)
    void refusesATranslocatedBundle() {
        const QString translocated = QStringLiteral("private/var/folders/xy/AppTranslocation/0A1B2C3D/d");
        bundle(translocated);
        const PackageInfo info = detectPackage(probe(translocated + QStringLiteral("/GitBolt.app/Contents/MacOS")));
        QCOMPARE(kindName(info.kind), kindName(PackageKind::MacBundle));
        QVERIFY(!info.canInstall);
        QVERIFY2(info.whyNot.contains(QLatin1String("temporary read-only copy")), qPrintable(info.whyNot));
    }

    void findsTheWindowsInstall() {
        QVERIFY(writeFile(path(QStringLiteral("win/GitBolt/Uninstall.exe")), "MZ"));
        QDir().mkpath(path(QStringLiteral("win/GitBolt/bin")));
        // UAC asks for elevation, so even a folder the user can't
        // write is installable.
        PackageProbe p = probe(QStringLiteral("win/GitBolt/bin"), {QStringLiteral("win/GitBolt")});
        PackageInfo info = detectPackage(p);
        QCOMPARE(kindName(info.kind), kindName(PackageKind::WindowsInstall));
        QCOMPARE(info.target, path(QStringLiteral("win/GitBolt")));
        QVERIFY(info.canInstall);

        // Registered as installed there: in either view, quoted or not,
        // in another case or with the other slashes.
        const QString own = path(QStringLiteral("win/GitBolt/Uninstall.exe"));
        QString backslashed = own;
        backslashed.replace(u'/', u'\\');
        for (const QStringList& registered :
             {QStringList{own}, QStringList{u'"' + own + u'"'}, QStringList{own.toUpper()},
              QStringList{backslashed}, QStringList{own, u'"' + own + QStringLiteral("\" /S")}}) {
            p.registeredUninstallers = registered;
            info = detectPackage(p);
            QCOMPARE(kindName(info.kind), kindName(PackageKind::WindowsInstall));
            QVERIFY2(info.canInstall, qPrintable(registered.join(u',') + QStringLiteral(": ") + info.whyNot));
        }
    }

    // A copy of the install folder (Uninstall.exe and all) isn't the
    // installed GitBolt the registry names: its update would uninstall
    // that one and install here instead.
    void refusesACopiedWindowsInstall() {
        QVERIFY(writeFile(path(QStringLiteral("copy/GitBolt/Uninstall.exe")), "MZ"));
        QDir().mkpath(path(QStringLiteral("copy/GitBolt/bin")));
        PackageProbe p = probe(QStringLiteral("copy/GitBolt/bin"));
        p.registeredUninstallers = {QStringLiteral("C:\\Program Files\\GitBolt\\Uninstall.exe")};
        PackageInfo info = detectPackage(p);
        QCOMPARE(kindName(info.kind), kindName(PackageKind::WindowsInstall));
        QVERIFY(!info.canInstall);
        QVERIFY2(info.whyNot.contains(QLatin1String("isn't the installed one, which is in C:\\Program Files\\GitBolt,")),
                 qPrintable(info.whyNot));
        QVERIFY2(info.whyNot.contains(QLatin1String("release page")), qPrintable(info.whyNot));

        // One view names this copy, the other the real install: the
        // installer could read either, so it is refused too.
        p.registeredUninstallers = {path(QStringLiteral("copy/GitBolt/Uninstall.exe")),
                                    QStringLiteral("\"C:\\Program Files\\GitBolt\\Uninstall.exe\"")};
        info = detectPackage(p);
        QVERIFY(!info.canInstall);
        QVERIFY2(info.whyNot.contains(QLatin1String("C:\\Program Files\\GitBolt")), qPrintable(info.whyNot));
    }

    void aWindowsBuildTreeIsUnknown() {
        QDir().mkpath(path(QStringLiteral("winbuild/bin")));
        PackageInfo info = detectPackage(probe(QStringLiteral("winbuild/bin")));
        QCOMPARE(kindName(info.kind), kindName(PackageKind::Unknown));
        // The uninstaller beside the exe, not above it, is no install.
        QVERIFY(writeFile(path(QStringLiteral("winbuild2/bin/Uninstall.exe")), "MZ"));
        info = detectPackage(probe(QStringLiteral("winbuild2/bin")));
        QCOMPARE(kindName(info.kind), kindName(PackageKind::Unknown));
    }

    void findsTheDeb() {
        QDir().mkpath(path(QStringLiteral("deb/usr/lib/gitbolt")));
        QVERIFY(writeFile(path(QStringLiteral("var/lib/dpkg/info/gitbolt.list")), "/usr/lib/gitbolt/gitbolt\n"));
        PackageProbe p = probe(QStringLiteral("deb/usr/lib/gitbolt"));
        p.pkexec = QStringLiteral("/usr/bin/pkexec");
        const PackageInfo info = detectPackage(p);
        QCOMPARE(kindName(info.kind), kindName(PackageKind::Deb));
        QCOMPARE(info.target, path(QStringLiteral("deb/usr/lib/gitbolt")));
        QVERIFY(info.canInstall);
    }

    void refusesADebWithoutPkexec() {
        QDir().mkpath(path(QStringLiteral("deb/usr/lib/gitbolt")));
        QVERIFY(writeFile(path(QStringLiteral("var/lib/dpkg/info/gitbolt.list")), "/usr/lib/gitbolt/gitbolt\n"));
        const PackageInfo info = detectPackage(probe(QStringLiteral("deb/usr/lib/gitbolt")));
        QCOMPARE(kindName(info.kind), kindName(PackageKind::Deb));
        QVERIFY(!info.canInstall);
        QVERIFY2(info.whyNot.contains(QLatin1String("sudo apt install ./gitbolt_<version>_amd64.deb")),
                 qPrintable(info.whyNot));
    }

    // The same layout without dpkg's list is a cmake --install prefix
    // (or the TGZ).
    void aDebLayoutDpkgDoesntOwnIsUnknown() {
        QDir().mkpath(path(QStringLiteral("prefix/usr/lib/gitbolt")));
        PackageProbe p = probe(QStringLiteral("prefix/usr/lib/gitbolt"));
        p.dpkgInfoDir = path(QStringLiteral("no/dpkg/here"));
        p.pkexec = QStringLiteral("/usr/bin/pkexec");
        QCOMPARE(kindName(detectPackage(p).kind), kindName(PackageKind::Unknown));
    }

    // Started from a terminal inside another AppImage (an IDE's, a
    // terminal emulator's), a deb install inherits its $APPIMAGE and
    // $APPDIR: still the deb, which an inherited variable doesn't make
    // anything else.
    void aDebStartedFromAnAppImagesTerminalIsStillTheDeb() {
        QDir().mkpath(path(QStringLiteral("deb2/usr/lib/gitbolt")));
        QVERIFY(writeFile(path(QStringLiteral("var/lib/dpkg/info/gitbolt.list")), "/usr/lib/gitbolt/gitbolt\n"));
        const QString other = path(QStringLiteral("home/Applications/Other.AppImage"));
        QVERIFY(writeFile(other, "\x7f" "ELF", true));
        QDir().mkpath(path(QStringLiteral("tmp/.mount_Other/usr/bin")));
        PackageProbe p = probe(QStringLiteral("deb2/usr/lib/gitbolt"));
        p.pkexec = QStringLiteral("/usr/bin/pkexec");
        p.appImage = other;
        p.appDir = path(QStringLiteral("tmp/.mount_Other"));
        const PackageInfo info = detectPackage(p);
        QCOMPARE(kindName(info.kind), kindName(PackageKind::Deb));
        QCOMPARE(info.target, path(QStringLiteral("deb2/usr/lib/gitbolt")));
        QVERIFY(info.canInstall);
    }

    void findsTheAppImage() {
        const QString image = path(QStringLiteral("apps/GitBolt-0.1.0-x86_64.AppImage"));
        QVERIFY(writeFile(image, "\x7f" "ELF", true));
        QDir().mkpath(path(QStringLiteral("mount/usr/bin")));
        PackageProbe p = probe(QStringLiteral("mount/usr/bin"));
        p.appImage = image;
        p.appDir = path(QStringLiteral("mount"));
        PackageInfo info = detectPackage(p);
        QCOMPARE(kindName(info.kind), kindName(PackageKind::AppImage));
        QCOMPARE(info.target, image);
        QVERIFY(info.canInstall);

        p = probe(QStringLiteral("mount/usr/bin"), {QStringLiteral("apps")});
        p.appImage = image;
        p.appDir = path(QStringLiteral("mount"));
        info = detectPackage(p);
        QCOMPARE(kindName(info.kind), kindName(PackageKind::AppImage));
        QVERIFY(!info.canInstall);
        QVERIFY2(info.whyNot.contains(QDir::toNativeSeparators(path(QStringLiteral("apps")))),
                 qPrintable(info.whyNot));

#if defined(Q_OS_UNIX)
        // $APPDIR reached through a link (a TMPDIR that is one), and a
        // trailing slash: the same folder, canonically.
        QVERIFY(QFile::link(path(QStringLiteral("mount")), path(QStringLiteral("mount-link"))));
        p = probe(QStringLiteral("mount/usr/bin"));
        p.appImage = image;
        p.appDir = path(QStringLiteral("mount-link")) + u'/';
        QCOMPARE(kindName(detectPackage(p).kind), kindName(PackageKind::AppImage));
#endif
    }

    // A hand-extracted GitBolt (squashfs-root/AppRun) started from a
    // shell inside another AppImage inherits that one's $APPIMAGE and
    // $APPDIR. That $APPIMAGE is another application, which an update
    // must never rename GitBolt over: this binary isn't in its $APPDIR.
    void anInheritedAppImageIsntThisOne() {
        QDir().mkpath(path(QStringLiteral("home/squashfs-root/usr/bin")));
        const QString other = path(QStringLiteral("home/Applications/Other.AppImage"));
        QVERIFY(writeFile(other, "\x7f" "ELF", true));
        QDir().mkpath(path(QStringLiteral("tmp/.mount_Other/usr/bin")));
        PackageProbe p = probe(QStringLiteral("home/squashfs-root/usr/bin"));
        p.appImage = other;
        p.appDir = path(QStringLiteral("tmp/.mount_Other"));
        PackageInfo info = detectPackage(p);
        QCOMPARE(kindName(info.kind), kindName(PackageKind::Unknown));
        QVERIFY(!info.canInstall);
        QVERIFY(info.target.isEmpty());
        // No $APPDIR at all, or one that doesn't exist: not an AppImage
        // either.
        p.appDir.clear();
        QCOMPARE(kindName(detectPackage(p).kind), kindName(PackageKind::Unknown));
        p.appDir = path(QStringLiteral("tmp/.mount_Gone"));
        QCOMPARE(kindName(detectPackage(p).kind), kindName(PackageKind::Unknown));
    }

    // Extracted by hand (squashfs-root, no $APPIMAGE), or $APPIMAGE
    // naming a file that is gone, or a binary outside usr/bin.
    void anAppImageWithoutItsRuntimeIsUnknown() {
        QDir().mkpath(path(QStringLiteral("squashfs-root/usr/bin")));
        PackageProbe p = probe(QStringLiteral("squashfs-root/usr/bin"));
        QCOMPARE(kindName(detectPackage(p).kind), kindName(PackageKind::Unknown));
        p.appImage = path(QStringLiteral("gone/GitBolt.AppImage"));
        QCOMPARE(kindName(detectPackage(p).kind), kindName(PackageKind::Unknown));

        const QString image = path(QStringLiteral("apps2/GitBolt.AppImage"));
        QVERIFY(writeFile(image, "\x7f" "ELF", true));
        QDir().mkpath(path(QStringLiteral("elsewhere/bin")));
        p = probe(QStringLiteral("elsewhere/bin"));
        p.appImage = image;
        QCOMPARE(kindName(detectPackage(p).kind), kindName(PackageKind::Unknown));
    }

    void anEmptyProbeIsUnknown() {
        const PackageInfo info = detectPackage(PackageProbe{});
        QCOMPARE(kindName(info.kind), kindName(PackageKind::Unknown));
        QVERIFY(!info.canInstall);
        QVERIFY(!info.whyNot.isEmpty());
        QVERIFY(info.target.isEmpty());
    }

    void namesEachKindsAsset_data() {
        QTest::addColumn<PackageKind>("kind");
        QTest::addColumn<QString>("name");
        QTest::newRow("macOS") << PackageKind::MacBundle << QStringLiteral("GitBolt-0.1.10-Darwin-arm64.dmg");
        QTest::newRow("Windows") << PackageKind::WindowsInstall << QStringLiteral("GitBolt-0.1.10-Windows-AMD64.exe");
        QTest::newRow("deb") << PackageKind::Deb << QStringLiteral("gitbolt_0.1.10_amd64.deb");
        QTest::newRow("AppImage") << PackageKind::AppImage << QStringLiteral("GitBolt-0.1.10-x86_64.AppImage");
        QTest::newRow("unknown") << PackageKind::Unknown << QString();
    }

    void namesEachKindsAsset() {
        QFETCH(PackageKind, kind);
        QFETCH(QString, name);
        QCOMPARE(assetNameFor(kind, Version{0, 1, 10}), name);
    }

    // Reading the registry writes nothing to it, even with
    // administrator rights (CI's runner has them): no key that wasn't
    // there is there afterwards, in either view. QSettings made an empty
    // one; the 32-bit NSIS uninstaller never removed the 64-bit view's.
    // Checked on a key of this test's own (no other test can have made
    // it first, as one may GitBolt's own, which is checked too).
    void readingTheRegistryCreatesNothing() {
#if !defined(Q_OS_WIN)
        QVERIFY(registeredWindowsUninstallers().isEmpty());
        QSKIP("Windows only: reads the registry");
#else
        const QString own = QStringLiteral("SOFTWARE\\GitBolt-test-%1-%2")
                                .arg(QCoreApplication::applicationPid())
                                .arg(QDateTime::currentMSecsSinceEpoch());
        const QString gitBolt = QStringLiteral("SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\GitBolt");
        const auto exists = [](const QString& key, REGSAM view) {
            HKEY opened = nullptr;
            const std::wstring path = key.toStdWString();
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ | view, &opened) != ERROR_SUCCESS)
                return false;
            RegCloseKey(opened);
            return true;
        };
        const QList<REGSAM> views{REGSAM(KEY_WOW64_32KEY), REGSAM(KEY_WOW64_64KEY)};
        // Should the test fail, what it made goes (it is the test's own).
        auto removeOwn = qScopeGuard([&] {
            for (const REGSAM view : views)
                RegDeleteKeyExW(HKEY_LOCAL_MACHINE, own.toStdWString().c_str(), view, 0);
        });
        QList<bool> gitBoltBefore;
        for (const REGSAM view : views) {
            QVERIFY(!exists(own, view));
            gitBoltBefore << exists(gitBolt, view);
        }
        HANDLE token = nullptr;
        TOKEN_ELEVATION elevation = {};
        DWORD size = 0;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
            GetTokenInformation(token, TokenElevation, &elevation, static_cast<DWORD>(sizeof(elevation)), &size);
            CloseHandle(token);
        }
        qInfo("elevated: %s (without it, nothing could be created anyway)", elevation.TokenIsElevated ? "yes" : "no");

        QVERIFY(registeredWindowsUninstallers(own).isEmpty());
        const QStringList registered = registeredWindowsUninstallers();
        QCOMPARE(currentProbe().registeredUninstallers, registered);
        for (qsizetype i = 0; i < views.size(); ++i) {
            QVERIFY2(!exists(own, views.at(i)), qPrintable(own + QStringLiteral(" was created")));
            if (!gitBoltBefore.at(i))
                QVERIFY2(!exists(gitBolt, views.at(i)), qPrintable(gitBolt + QStringLiteral(" was created")));
        }
#endif
    }

    // This binary runs from the build tree, which is no package.
    void probesTheRunningProcess() {
        const PackageProbe p = currentProbe();
        QCOMPARE(p.appDirPath, QCoreApplication::applicationDirPath());
        QCOMPARE(p.appImage, qEnvironmentVariable("APPIMAGE"));
        QCOMPARE(p.appDir, qEnvironmentVariable("APPDIR"));
        QCOMPARE(p.registeredUninstallers, registeredWindowsUninstallers());
#if !defined(Q_OS_WIN)
        QVERIFY(p.registeredUninstallers.isEmpty());
#endif
        QVERIFY(!p.writable);
        QCOMPARE(kindName(detectPackage(p).kind), kindName(PackageKind::Unknown));
    }
};

QTEST_GUILESS_MAIN(TestPackage)
#include "TestPackage.moc"
