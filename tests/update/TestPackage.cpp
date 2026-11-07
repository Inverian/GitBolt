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
//   - a Windows install: bin\ beside the NSIS uninstaller; never
//     refused (the installer asks for elevation);
//   - a .deb: /usr/lib/gitbolt that dpkg lists, without $APPIMAGE;
//     refused without pkexec, and the refusal gives the command;
//   - an AppImage: $APPIMAGE an existing file and the binary in
//     usr/bin; refused where its folder can't be written;
//   - anything else (a build tree, a cmake --install prefix, a
//     hand-extracted AppImage) is Unknown, never installable;
//   - the release's asset name for each kind, and the probe of this
//     very test binary (a build tree: Unknown).
//

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include "FakeApps.h"
#include "update/Package.h"

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
        const PackageInfo info =
            detectPackage(probe(QStringLiteral("win/GitBolt/bin"), {QStringLiteral("win/GitBolt")}));
        QCOMPARE(kindName(info.kind), kindName(PackageKind::WindowsInstall));
        QCOMPARE(info.target, path(QStringLiteral("win/GitBolt")));
        QVERIFY(info.canInstall);
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
    // (or the TGZ); with $APPIMAGE set it is something else again.
    void aDebLayoutDpkgDoesntOwnIsUnknown() {
        QDir().mkpath(path(QStringLiteral("prefix/usr/lib/gitbolt")));
        PackageProbe p = probe(QStringLiteral("prefix/usr/lib/gitbolt"));
        p.dpkgInfoDir = path(QStringLiteral("no/dpkg/here"));
        p.pkexec = QStringLiteral("/usr/bin/pkexec");
        QCOMPARE(kindName(detectPackage(p).kind), kindName(PackageKind::Unknown));

        QVERIFY(writeFile(path(QStringLiteral("var/lib/dpkg/info/gitbolt.list")), "/usr/lib/gitbolt/gitbolt\n"));
        p.dpkgInfoDir = path(QStringLiteral("var/lib/dpkg/info"));
        p.appImage = path(QStringLiteral("nowhere/GitBolt.AppImage"));
        QCOMPARE(kindName(detectPackage(p).kind), kindName(PackageKind::Unknown));
    }

    void findsTheAppImage() {
        const QString image = path(QStringLiteral("apps/GitBolt-0.1.0-x86_64.AppImage"));
        QVERIFY(writeFile(image, "\x7f" "ELF", true));
        QDir().mkpath(path(QStringLiteral("mount/usr/bin")));
        PackageProbe p = probe(QStringLiteral("mount/usr/bin"));
        p.appImage = image;
        PackageInfo info = detectPackage(p);
        QCOMPARE(kindName(info.kind), kindName(PackageKind::AppImage));
        QCOMPARE(info.target, image);
        QVERIFY(info.canInstall);

        p = probe(QStringLiteral("mount/usr/bin"), {QStringLiteral("apps")});
        p.appImage = image;
        info = detectPackage(p);
        QCOMPARE(kindName(info.kind), kindName(PackageKind::AppImage));
        QVERIFY(!info.canInstall);
        QVERIFY2(info.whyNot.contains(QDir::toNativeSeparators(path(QStringLiteral("apps")))),
                 qPrintable(info.whyNot));
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

    // This binary runs from the build tree, which is no package.
    void probesTheRunningProcess() {
        const PackageProbe p = currentProbe();
        QCOMPARE(p.appDirPath, QCoreApplication::applicationDirPath());
        QCOMPARE(p.appImage, qEnvironmentVariable("APPIMAGE"));
        QVERIFY(!p.writable);
        QCOMPARE(kindName(detectPackage(p).kind), kindName(PackageKind::Unknown));
    }
};

QTEST_GUILESS_MAIN(TestPackage)
#include "TestPackage.moc"
