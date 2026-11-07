//
// TestInstaller — getting a downloaded installer in place
// (update/Installer.h): prepare() while GitBolt runs, launchHelper()
// just before it quits.
//
//   - every package: a copy that can't install, or a download that
//     isn't this package's asset for that version (another name, a
//     link, nothing there), is refused, and every outcome arrives from
//     the event loop, never inside prepare();
//   - the .deb and the Windows installer need nothing staged; the
//     AppImage must have been downloaded beside the old one (the helper
//     renames it over it) and is made executable;
//   - launchHelper() only after prepared(), and cancel() discards a
//     prepared update;
//   - Unix, end to end: an AppImage update with a real detached helper,
//     which waits for a real pid, replaces the file, and starts the new
//     one in a session of its own with the test's environment;
//   - macOS, with real disk images made here (hdiutil create) from
//     fake ad-hoc-signed bundles: a good one is attached read-only,
//     checked, copied beside the installed bundle as .GitBolt-<v>.app,
//     checked again and detached; a wrong bundle id, a wrong version
//     and a broken seal are refused, leaving nothing staged or mounted;
//     cancel() mid-copy undoes it all; and the whole update, helper and
//     all, swaps the bundle, moves the old one to (a scratch HOME's)
//     Trash and starts the new one. A bundle run from a mounted disk
//     image (/Volumes/…) is refused by detectPackage.
//
// Nothing here touches /Applications, the real Trash or an installed
// GitBolt: every bundle is in a temporary folder, and HOME is one too.
//

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include "../TestProcessHelper.h"
#include "FakeApps.h"
#include "update/Installer.h"
#include "update/Package.h"

#include <memory>

#if defined(Q_OS_UNIX)
#include <unistd.h>
#endif

using namespace gitbolt::update;
using gitbolt::test::fakeAppScript;
using gitbolt::test::makeBundle;
using gitbolt::test::readFile;
using gitbolt::test::runTool;
using gitbolt::test::ScopedEnv;
using gitbolt::test::writeFile;

namespace {

const Version kOld{0, 1, 0};
const Version kNew{0, 1, 1};
const QString kInstance = QStringLiteral("gitbolt-installer-test");

struct Outcome {
    bool prepared = false;
    QString failed;
    QStringList statuses;
};

// Runs prepare() to its end (or `timeoutMs`).
Outcome prepareAndWait(Installer& installer, const QString& file, const Version& version, int timeoutMs = 60000) {
    Outcome outcome;
    bool done = false;
    const auto a = QObject::connect(&installer, &Installer::prepared, [&] {
        outcome.prepared = true;
        done = true;
    });
    const auto b = QObject::connect(&installer, &Installer::failed, [&](const QString& message) {
        outcome.failed = message;
        done = true;
    });
    const auto c = QObject::connect(&installer, &Installer::status,
                                    [&](const QString& text) { outcome.statuses << text; });
    installer.prepare(file, version);
    // Never from inside prepare().
    if (done)
        outcome.failed = QStringLiteral("signalled from inside prepare()");
    else if (!QTest::qWaitFor([&] { return done; }, timeoutMs))
        outcome.failed = QStringLiteral("neither prepared() nor failed() in time");
    QObject::disconnect(a);
    QObject::disconnect(b);
    QObject::disconnect(c);
    return outcome;
}

QString copyOf(const QString& bundle) {
    return readFile(bundle + QStringLiteral("/Contents/Resources/copy.txt")).trimmed();
}

} // namespace

class TestInstaller : public QObject {
    Q_OBJECT

private:
    std::unique_ptr<QTemporaryDir> dir_;
    QTemporaryDir images_;   // disk images, made once and reused
    std::unique_ptr<ScopedEnv> home_;
    QString homePath_;
    std::unique_ptr<QProcess> waited_;

    QString path(const QString& relative) const { return QDir::cleanPath(dir_->filePath(relative)); }
    QString workDir() const { return path(QStringLiteral("cache/updates")); }
    QString marker() const { return path(QStringLiteral("marker.txt")); }

    qint64 startWaited() {
        waited_ = std::make_unique<QProcess>();
        waited_->start(QStringLiteral("/bin/sleep"), {QStringLiteral("60")});
        return waited_->waitForStarted() ? waited_->processId() : 0;
    }

    void quitWaited() {
        waited_->kill();
        waited_->waitForFinished();
    }

    QString diagnostics() const {
        return QStringLiteral("install.log:\n%1\nmarker:\n%2")
            .arg(readFile(QDir(workDir()).filePath(QStringLiteral("install.log"))), readFile(marker()));
    }

    // A download named as the release names it, in its version's folder.
    QString download(PackageKind kind, const QByteArray& content = "installer") const {
        const QString file = QDir(workDir()).filePath(QStringLiteral("0.1.1/") + assetNameFor(kind, kNew));
        return writeFile(file, content) ? file : QString();
    }

    // macOS: the installed copy, <dir>/Applications/GitBolt.app at
    // 0.1.0 with a file planted in it.
    PackageInfo installedBundle() const {
        const QString target = path(QStringLiteral("Applications/GitBolt.app"));
        if (!makeBundle(target, kOld.toString())
            || !writeFile(target + QStringLiteral("/Contents/Resources/planted.txt"), "old\n"))
            return {};
        return {PackageKind::MacBundle, target, true, {}};
    }

    // macOS: a disk image laid out like the release's (GitBolt.app and
    // an Applications link) holding a bundle of `version` and `id`,
    // signed ad hoc; `tamper` changes a sealed file after signing.
    // Made once per case for the whole run.
    QString diskImage(const QString& name, const QString& version, const QString& id, bool tamper = false) {
        const QString dmg = images_.filePath(name + QStringLiteral("/") + assetNameFor(PackageKind::MacBundle, kNew));
        if (QFileInfo::exists(dmg))
            return dmg;
        const QString folder = images_.filePath(name + QStringLiteral("/content"));
        const QString app = folder + QStringLiteral("/GitBolt.app");
        if (!makeBundle(app, version, id) || !gitbolt::test::adHocSign(app))
            return {};
        if (tamper && !writeFile(app + QStringLiteral("/Contents/Resources/copy.txt"), "changed after signing\n"))
            return {};
        QFile::link(QStringLiteral("/Applications"), folder + QStringLiteral("/Applications"));
        return gitbolt::test::makeDiskImage(folder, dmg) ? dmg : QString();
    }

    // No disk image of this test's is still attached.
    static bool nothingMountedUnder(const QString& folder) {
        QString info;
        runTool(QStringLiteral("/usr/bin/hdiutil"), {QStringLiteral("info")}, &info);
        const QString canonical = QFileInfo(folder).canonicalFilePath();
        return !info.contains(folder) && (canonical.isEmpty() || !info.contains(canonical));
    }

    QStringList mountPoints() const {
        return QDir(workDir()).entryList({QStringLiteral("mount-*")}, QDir::AllEntries | QDir::Hidden);
    }

private slots:
    void initTestCase() {
        QVERIFY(images_.isValid());
#if defined(Q_OS_UNIX)
        // The helper puts the old bundle in $HOME/.Trash: a scratch one.
        static QTemporaryDir home;
        QVERIFY(home.isValid());
        homePath_ = home.path();
        QVERIFY(QDir().mkpath(home.filePath(QStringLiteral(".Trash"))));
        home_ = std::make_unique<ScopedEnv>("HOME", QFile::encodeName(homePath_));
        QCOMPARE(QDir::homePath(), homePath_);
#endif
    }

    void init() {
        dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(dir_->isValid());
        QVERIFY(!dir_->path().startsWith(QLatin1String("/Applications")));
    }

    void cleanup() {
        if (waited_ && waited_->state() != QProcess::NotRunning)
            quitWaited();
        waited_.reset();
        dir_.reset();
    }

    // ---------------------------------------------------------------
    // Every package
    // ---------------------------------------------------------------

    void refusesWhatThisCopyCantInstall() {
        Installer installer({PackageKind::Unknown, {}, false, QStringLiteral("Not from a package.")}, kOld, workDir());
        const Outcome outcome = prepareAndWait(installer, download(PackageKind::Deb), kNew);
        QVERIFY(!outcome.prepared);
        QCOMPARE(outcome.failed, QStringLiteral("Not from a package."));
        QVERIFY(!installer.busy());

        Installer deb({PackageKind::Deb, QStringLiteral("/usr/lib/gitbolt"), false, QStringLiteral("No pkexec.")},
                      kOld, workDir());
        QCOMPARE(prepareAndWait(deb, download(PackageKind::Deb), kNew).failed, QStringLiteral("No pkexec."));
    }

    void refusesADownloadThatIsntThisPackagesAsset() {
        Installer installer({PackageKind::Deb, QStringLiteral("/usr/lib/gitbolt"), true, {}}, kOld, workDir());
        // Another version's name, another package's, nothing there.
        const QString other = QDir(workDir()).filePath(QStringLiteral("0.1.1/gitbolt_0.1.2_amd64.deb"));
        QVERIFY(writeFile(other, "x"));
        Outcome outcome = prepareAndWait(installer, other, kNew);
        QVERIFY(!outcome.prepared);
        QVERIFY2(outcome.failed.contains(QLatin1String("gitbolt_0.1.1_amd64.deb")), qPrintable(outcome.failed));
        outcome = prepareAndWait(installer, download(PackageKind::AppImage), kNew);
        QVERIFY(!outcome.failed.isEmpty());
        outcome = prepareAndWait(installer, QDir(workDir()).filePath(QStringLiteral("gone/gitbolt_0.1.1_amd64.deb")),
                                 kNew);
        QVERIFY(!outcome.failed.isEmpty());
#if defined(Q_OS_UNIX)
        // A link of the right name to something else.
        const QString link = QDir(workDir()).filePath(QStringLiteral("link/gitbolt_0.1.1_amd64.deb"));
        QVERIFY(QDir().mkpath(QFileInfo(link).absolutePath()));
        QVERIFY(QFile::link(other, link));
        outcome = prepareAndWait(installer, link, kNew);
        QVERIFY(!outcome.failed.isEmpty());
#endif
    }

    void debAndWindowsNeedNothingStaged_data() {
        QTest::addColumn<int>("kind");
        QTest::newRow("deb") << int(PackageKind::Deb);
        QTest::newRow("Windows") << int(PackageKind::WindowsInstall);
    }

    void debAndWindowsNeedNothingStaged() {
        QFETCH(int, kind);
        const auto packageKind = static_cast<PackageKind>(kind);
        Installer installer({packageKind, path(QStringLiteral("install")), true, {}}, kOld, workDir());
        const QString file = download(packageKind);
        const Outcome outcome = prepareAndWait(installer, file, kNew);
        QVERIFY2(outcome.prepared, qPrintable(outcome.failed));
        QVERIFY(outcome.statuses.isEmpty());
        QCOMPARE(readFile(file), QStringLiteral("installer"));
        QCOMPARE(QDir(QFileInfo(file).absolutePath()).entryList(QDir::Files | QDir::Hidden).size(), 1);
    }

    void appImageIsMadeExecutableBesideTheOldOne() {
        const QString target = path(QStringLiteral("apps/GitBolt-0.1.0-x86_64.AppImage"));
        QVERIFY(writeFile(target, fakeAppScript(QStringLiteral("0.1.0")), true));
        Installer installer({PackageKind::AppImage, target, true, {}}, kOld, workDir());

        // Downloaded anywhere else, it can't be renamed over the old one.
        Outcome outcome = prepareAndWait(installer, download(PackageKind::AppImage), kNew);
        QVERIFY(!outcome.prepared);
        QVERIFY2(outcome.failed.contains(QLatin1String("next to the old one")), qPrintable(outcome.failed));

        const QString beside = path(QStringLiteral("apps/GitBolt-0.1.1-x86_64.AppImage"));
        QVERIFY(writeFile(beside, fakeAppScript(QStringLiteral("0.1.1"))));
        outcome = prepareAndWait(installer, beside, kNew);
        QVERIFY2(outcome.prepared, qPrintable(outcome.failed));
#if defined(Q_OS_UNIX)
        QCOMPARE(QFileInfo(beside).permissions() & (QFile::ExeOwner | QFile::ExeGroup | QFile::ExeOther),
                 QFile::ExeOwner | QFile::ExeGroup | QFile::ExeOther);
#endif
        // The running AppImage, renamed to the new one's name, is not
        // replaced with itself.
        Installer self({PackageKind::AppImage, beside, true, {}}, kOld, workDir());
        outcome = prepareAndWait(self, beside, kNew);
        QVERIFY(!outcome.prepared);
        QVERIFY2(outcome.failed.contains(QLatin1String("itself")), qPrintable(outcome.failed));
    }

    void launchesOnlyAPreparedUpdate() {
        Installer installer({PackageKind::Deb, QStringLiteral("/usr/lib/gitbolt"), true, {}}, kOld, workDir());
        QSignalSpy failed(&installer, &Installer::failed);
        QVERIFY(!installer.launchHelper(12345));
        QCOMPARE(failed.size(), 1);

        // Prepared, then cancelled: nothing to launch either.
        QVERIFY(prepareAndWait(installer, download(PackageKind::Deb), kNew).prepared);
        installer.cancel();
        QVERIFY(!installer.launchHelper(12345));
        QCOMPARE(failed.size(), 2);
        QVERIFY(!QFileInfo::exists(installer.helperScriptPath()));
    }

    // cancel() before the event loop runs: no signal at all.
    void cancelledAtOnceSaysNothing() {
        Installer installer({PackageKind::Deb, QStringLiteral("/usr/lib/gitbolt"), true, {}}, kOld, workDir());
        QSignalSpy prepared(&installer, &Installer::prepared);
        QSignalSpy failed(&installer, &Installer::failed);
        installer.prepare(download(PackageKind::Deb), kNew);
        installer.cancel();
        QTest::qWait(200);
        QCOMPARE(prepared.size(), 0);
        QCOMPARE(failed.size(), 0);
        QVERIFY(!installer.busy());
    }

    // ---------------------------------------------------------------
    // Unix, end to end
    // ---------------------------------------------------------------

    void installsAnAppImageEndToEnd() {
#if !defined(Q_OS_UNIX)
        QSKIP("POSIX only");
#else
        const QString target = path(QStringLiteral("My Apps/GitBolt.AppImage"));
        QVERIFY(writeFile(target, fakeAppScript(QStringLiteral("0.1.0")), true));
        const QString beside = path(QStringLiteral("My Apps/GitBolt-0.1.1-x86_64.AppImage"));
        QVERIFY(writeFile(beside, fakeAppScript(QStringLiteral("0.1.1"))));
        Installer installer({PackageKind::AppImage, target, true, {}}, kOld, workDir());
        const Outcome outcome = prepareAndWait(installer, beside, kNew);
        QVERIFY2(outcome.prepared, qPrintable(outcome.failed));

        ScopedEnv markerEnv("GITBOLT_TEST_MARKER", QFile::encodeName(marker()));
        ScopedEnv instance("GITBOLT_INSTANCE_NAME", kInstance.toUtf8());
        const qint64 pid = startWaited();
        QVERIFY(pid > 0);
        QVERIFY(installer.launchHelper(pid));
        QVERIFY(QFileInfo::exists(installer.helperScriptPath()));
        // Launched once: the helper owns the update now.
        QVERIFY(!installer.launchHelper(pid));

        QVERIFY2(gitbolt::test::waitForLines(installer.logPath(), 2, 10000), qPrintable(diagnostics()));
        QTest::qWait(300);
        QVERIFY(readFile(target).contains(QLatin1String("'0.1.0'")));
        QVERIFY(!QFileInfo::exists(marker()));

        quitWaited();
        QVERIFY2(gitbolt::test::waitForLines(marker(), 1, 20000), qPrintable(diagnostics()));
        QVERIFY(readFile(target).contains(QLatin1String("'0.1.1'")));
        QVERIFY(!QFileInfo::exists(beside));
        // Started by a detached helper in a session (so a process
        // group) of its own, with the instance name passed on.
        const QString started = readFile(marker());
        const QRegularExpressionMatch m =
            QRegularExpression(QStringLiteral("^0\\.1\\.1 pid=(\\d+) pgid=(\\d+) instance=(\\S+)\\n$")).match(started);
        QVERIFY2(m.hasMatch(), qPrintable(started));
        QVERIFY(m.captured(2).toLongLong() != qint64(::getpgrp()));
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
        // CreateNewSession: the helper leads its session itself (before
        // 6.7 startDetached's intermediate child does).
        QCOMPARE(m.captured(1), m.captured(2));
#endif
        QCOMPARE(m.captured(3), kInstance);
        QVERIFY2(readFile(installer.logPath()).contains(QLatin1String("installed the new AppImage")),
                 qPrintable(diagnostics()));
#endif
    }

    // ---------------------------------------------------------------
    // macOS, with real disk images
    // ---------------------------------------------------------------

    void preparesARealDiskImage() {
#if !defined(Q_OS_MACOS)
        QSKIP("macOS only");
#else
        const QString dmg = diskImage(QStringLiteral("good"), kNew.toString(), QStringLiteral("com.gitbolt.app"));
        QVERIFY(!dmg.isEmpty());
        const PackageInfo package = installedBundle();
        QVERIFY(!package.target.isEmpty());
        auto installer = std::make_unique<Installer>(package, kOld, workDir());
        Outcome outcome = prepareAndWait(*installer, dmg, kNew);
        QVERIFY2(outcome.prepared, qPrintable(outcome.failed));
        QCOMPARE(outcome.statuses, (QStringList{QStringLiteral("Checking the download…"),
                                                QStringLiteral("Copying GitBolt…")}));

        const QString staged = path(QStringLiteral("Applications/.GitBolt-0.1.1.app"));
        QCOMPARE(copyOf(staged), QStringLiteral("0.1.1"));
        QString output;
        QVERIFY2(runTool(QStringLiteral("/usr/bin/codesign"),
                         {QStringLiteral("--verify"), QStringLiteral("--deep"), QStringLiteral("--strict"), staged},
                         &output)
                     == 0,
                 qPrintable(output));
        // The installed copy untouched until the helper runs; the disk
        // image detached, its mount point gone.
        QCOMPARE(copyOf(package.target), QStringLiteral("0.1.0"));
        QVERIFY(mountPoints().isEmpty());
        QVERIFY(nothingMountedUnder(workDir()));
        QVERIFY(!installer->busy());

        // Prepared again (a retry): the copy left from the first time is
        // replaced, not refused.
        QVERIFY(writeFile(staged + QStringLiteral("/Contents/Resources/stale.txt"), "x"));
        outcome = prepareAndWait(*installer, dmg, kNew);
        QVERIFY2(outcome.prepared, qPrintable(outcome.failed));
        QVERIFY(!QFileInfo::exists(staged + QStringLiteral("/Contents/Resources/stale.txt")));
        QCOMPARE(copyOf(staged), QStringLiteral("0.1.1"));

        // An Installer that goes away unlaunched takes its copy with it.
        installer.reset();
        QVERIFY(!QFileInfo::exists(staged));
        QCOMPARE(copyOf(package.target), QStringLiteral("0.1.0"));
#endif
    }

    void refusesABadDiskImage_data() {
        QTest::addColumn<QString>("name");
        QTest::addColumn<QString>("version");
        QTest::addColumn<QString>("id");
        QTest::addColumn<bool>("tamper");
        QTest::addColumn<QString>("reason");
        QTest::newRow("another app") << QStringLiteral("wrong-id") << QStringLiteral("0.1.1")
                                     << QStringLiteral("com.example.notgitbolt") << false
                                     << QStringLiteral("isn't GitBolt: its identifier is com.example.notgitbolt");
        QTest::newRow("another version") << QStringLiteral("wrong-version") << QStringLiteral("0.1.9")
                                         << QStringLiteral("com.gitbolt.app") << false
                                         << QStringLiteral("is version 0.1.9, not 0.1.1");
        QTest::newRow("broken seal") << QStringLiteral("tampered") << QStringLiteral("0.1.1")
                                     << QStringLiteral("com.gitbolt.app") << true
                                     << QStringLiteral("fails its signature check");
    }

    void refusesABadDiskImage() {
#if !defined(Q_OS_MACOS)
        QSKIP("macOS only");
#else
        QFETCH(QString, name);
        QFETCH(QString, version);
        QFETCH(QString, id);
        QFETCH(bool, tamper);
        QFETCH(QString, reason);
        const QString dmg = diskImage(name, version, id, tamper);
        QVERIFY(!dmg.isEmpty());
        const PackageInfo package = installedBundle();
        Installer installer(package, kOld, workDir());
        const Outcome outcome = prepareAndWait(installer, dmg, kNew);
        QVERIFY(!outcome.prepared);
        QVERIFY2(outcome.failed.contains(reason), qPrintable(outcome.failed));
        // Nothing staged, nothing left mounted, the installed copy as
        // it was.
        QCOMPARE(QDir(path(QStringLiteral("Applications")))
                     .entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot),
                 QStringList{QStringLiteral("GitBolt.app")});
        QCOMPARE(copyOf(package.target), QStringLiteral("0.1.0"));
        QVERIFY(mountPoints().isEmpty());
        QVERIFY(nothingMountedUnder(workDir()));
#endif
    }

    void cancelUndoesAHalfDonePrepare_data() {
        QTest::addColumn<QString>("step");
        QTest::addColumn<bool>("fromTheSlot");
        // A slot of status() itself cancels: nothing after the signal
        // may carry on.
        QTest::newRow("checking, from the slot") << QStringLiteral("Checking the download…") << true;
        QTest::newRow("copying, from the slot") << QStringLiteral("Copying GitBolt…") << true;
        // Cancelled from the event loop while hdiutil attaches or ditto
        // copies.
        QTest::newRow("while attaching") << QStringLiteral("Checking the download…") << false;
        QTest::newRow("while copying") << QStringLiteral("Copying GitBolt…") << false;
    }

    // Cancelled half-way: the copy so far is removed, the image
    // detached, and neither prepared() nor failed() comes.
    void cancelUndoesAHalfDonePrepare() {
#if !defined(Q_OS_MACOS)
        QSKIP("macOS only");
#else
        QFETCH(QString, step);
        QFETCH(bool, fromTheSlot);
        const QString dmg = diskImage(QStringLiteral("good"), kNew.toString(), QStringLiteral("com.gitbolt.app"));
        QVERIFY(!dmg.isEmpty());
        const PackageInfo package = installedBundle();
        Installer installer(package, kOld, workDir());
        QSignalSpy prepared(&installer, &Installer::prepared);
        QSignalSpy failed(&installer, &Installer::failed);
        bool copying = false;
        connect(&installer, &Installer::status, this, [&](const QString& text) {
            if (text != step)
                return;
            copying = true;
            if (fromTheSlot)
                installer.cancel();
            else
                QTimer::singleShot(0, &installer, [&] { installer.cancel(); });
        });
        installer.prepare(dmg, kNew);
        QVERIFY(QTest::qWaitFor([&] { return copying; }, 60000));
        QTest::qWait(1000);
        QCOMPARE(prepared.size(), 0);
        QCOMPARE(failed.size(), 0);
        QVERIFY(!installer.busy());
        QVERIFY(!QFileInfo::exists(path(QStringLiteral("Applications/.GitBolt-0.1.1.app"))));
        QVERIFY(mountPoints().isEmpty());
        QVERIFY(nothingMountedUnder(workDir()));
#endif
    }

    // The whole update: prepare, the helper, GitBolt quits, the bundle
    // is swapped, the old one is in the Trash, the new one starts.
    void installsADiskImageEndToEnd() {
#if !defined(Q_OS_MACOS)
        QSKIP("macOS only");
#else
        const QString dmg = diskImage(QStringLiteral("good"), kNew.toString(), QStringLiteral("com.gitbolt.app"));
        QVERIFY(!dmg.isEmpty());
        const PackageInfo package = installedBundle();
        Installer installer(package, kOld, workDir());
        const Outcome outcome = prepareAndWait(installer, dmg, kNew);
        QVERIFY2(outcome.prepared, qPrintable(outcome.failed));

        ScopedEnv markerEnv("GITBOLT_TEST_MARKER", QFile::encodeName(marker()));
        ScopedEnv instance("GITBOLT_INSTANCE_NAME", kInstance.toUtf8());
        const qint64 pid = startWaited();
        QVERIFY(installer.launchHelper(pid));
        const QString script = readFile(installer.helperScriptPath());
        QVERIFY(script.contains(QStringLiteral("\ntrash='%1/.Trash'\n").arg(homePath_)));
        QVERIFY(script.contains(QLatin1String("\nexec_directly=1\n")));
        QVERIFY2(gitbolt::test::waitForLines(installer.logPath(), 2, 10000), qPrintable(diagnostics()));
        QTest::qWait(300);
        QCOMPARE(copyOf(package.target), QStringLiteral("0.1.0"));

        quitWaited();
        QVERIFY2(gitbolt::test::waitForLines(marker(), 1, 20000), qPrintable(diagnostics()));
        QCOMPARE(copyOf(package.target), QStringLiteral("0.1.1"));
        QString output;
        QVERIFY2(runTool(QStringLiteral("/usr/bin/codesign"),
                         {QStringLiteral("--verify"), QStringLiteral("--deep"), QStringLiteral("--strict"),
                          package.target},
                         &output)
                     == 0,
                 qPrintable(output));
        QVERIFY(!QFileInfo::exists(package.target + QStringLiteral("/Contents/Resources/planted.txt")));
        const QString trashed = homePath_ + QStringLiteral("/.Trash/GitBolt-0.1.0.app");
        QVERIFY2(QFileInfo::exists(trashed + QStringLiteral("/Contents/Resources/planted.txt")), qPrintable(diagnostics()));
        QCOMPARE(QDir(path(QStringLiteral("Applications")))
                     .entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot),
                 QStringList{QStringLiteral("GitBolt.app")});
        const QRegularExpressionMatch m =
            QRegularExpression(QStringLiteral("^0\\.1\\.1 pid=(\\d+) pgid=(\\d+) instance=(\\S+)\\n$"))
                .match(readFile(marker()));
        QVERIFY2(m.hasMatch(), qPrintable(readFile(marker())));
        QVERIFY(m.captured(2).toLongLong() != qint64(::getpgrp()));
        QCOMPARE(m.captured(3), kInstance);
        QVERIFY(QDir(trashed).removeRecursively());
#endif
    }

    // Run from the disk image itself (mounted where Finder mounts it,
    // under /Volumes): detectPackage refuses to update that copy.
    void refusesABundleOnADiskImage() {
#if !defined(Q_OS_MACOS)
        QSKIP("macOS only");
#else
        const QString dmg = diskImage(QStringLiteral("good"), kNew.toString(), QStringLiteral("com.gitbolt.app"));
        QVERIFY(!dmg.isEmpty());
        QString output;
        int status = -1;
        for (int attempt = 1; attempt <= 5 && status != 0; ++attempt) {
            if (attempt > 1)
                QTest::qWait(attempt * 1000);
            status = runTool(QStringLiteral("/usr/bin/hdiutil"),
                             {QStringLiteral("attach"), QStringLiteral("-readonly"), QStringLiteral("-nobrowse"),
                              QStringLiteral("-noautoopen"), dmg},
                             &output);
        }
        QVERIFY2(status == 0, qPrintable(output));
        QString volume;
        for (const QString& line : output.split(u'\n')) {
            const qsizetype at = line.indexOf(QLatin1String("/Volumes/"));
            if (at >= 0)
                volume = line.mid(at).trimmed();
        }
        QVERIFY2(!volume.isEmpty(), qPrintable(output));
        auto detach = qScopeGuard([&] {
            if (runTool(QStringLiteral("/usr/bin/hdiutil"), {QStringLiteral("detach"), volume}) != 0)
                runTool(QStringLiteral("/usr/bin/hdiutil"), {QStringLiteral("detach"), QStringLiteral("-force"), volume});
        });
        PackageProbe probe;
        probe.appDirPath = volume + QStringLiteral("/GitBolt.app/Contents/MacOS");
        probe.writable = [](const QString&) { return true; };
        const PackageInfo info = detectPackage(probe);
        QCOMPARE(int(info.kind), int(PackageKind::MacBundle));
        QVERIFY(!info.canInstall);
        QVERIFY2(info.whyNot.contains(QLatin1String("disk image")), qPrintable(info.whyNot));
#endif
    }
};

QTEST_GUILESS_MAIN(TestInstaller)
#include "TestInstaller.moc"
