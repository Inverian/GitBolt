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
//     which waits for a real pid, replaces the file, starts the new
//     one in a session of its own with the test's environment, and
//     leaves "ok" in install-result (an earlier result is gone first);
//   - macOS, with real disk images made here (hdiutil create) from
//     fake ad-hoc-signed bundles: a good one is attached read-only,
//     checked, copied beside the installed bundle as .GitBolt-<v>.app,
//     checked again and detached; a wrong bundle id, a wrong version,
//     a broken seal and a minimum macOS above this one are refused,
//     leaving nothing staged or mounted; cancel() mid-copy returns at
//     once and the background undoes it all, without ever reaching the
//     next prepare()'s copy; and the whole update, helper and all,
//     swaps the bundle, moves the old one to (a scratch HOME's) Trash
//     and starts the new one. A bundle run from a mounted disk image
//     (/Volumes/…) is refused by detectPackage;
//   - removeStaleInstallFiles: staged and swapped-aside copies beside
//     the bundle, mount points in the work folder (a disk image still
//     mounted there detached), the AppImage's .part files, and the one
//     AppImage download GitBolt recorded (recordPendingDownload) while
//     it is still that file, go; the user's own GitBolt AppImages of
//     any version, the running copy, anything not named exactly so,
//     and links (and what they point to) stay;
//   - a release that needs a newer macOS than this one is refused, and
//     needsNewerSystem() says so (the UI then doesn't download it
//     again).
//
// Nothing here touches /Applications, the real Trash or an installed
// GitBolt: every bundle is in a temporary folder, and HOME is one too.
//

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStorageInfo>
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
    // signed ad hoc; `tamper` changes a sealed file after signing;
    // `minimumSystem` is its LSMinimumSystemVersion. Made once per case
    // for the whole run.
    QString diskImage(const QString& name, const QString& version, const QString& id, bool tamper = false,
                      const QString& minimumSystem = QStringLiteral("11.0")) {
        const QString dmg = images_.filePath(name + QStringLiteral("/") + assetNameFor(PackageKind::MacBundle, kNew));
        if (QFileInfo::exists(dmg))
            return dmg;
        const QString folder = images_.filePath(name + QStringLiteral("/content"));
        const QString app = folder + QStringLiteral("/GitBolt.app");
        if (!makeBundle(app, version, id, {}, minimumSystem) || !gitbolt::test::adHocSign(app))
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

    // A disk image is mounted at one of workDir's mount points, by the
    // file system (statfs), at once: no hdiutil to wait for.
    bool mountedNow() const {
        for (const QString& name : mountPoints()) {
            const QString canonical = QFileInfo(QDir(workDir()).filePath(name)).canonicalFilePath();
            const QStorageInfo volume(canonical);
            if (!canonical.isEmpty() && volume.isValid()
                && QFileInfo(volume.rootPath()).canonicalFilePath() == canonical)
                return true;
        }
        return false;
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
        // An earlier update's result, which mustn't pass for this one's.
        QCOMPARE(installer.resultPath(), QDir(workDir()).filePath(QStringLiteral("install-result")));
        QVERIFY(writeFile(installer.resultPath(), "failed 0.1.1 an earlier attempt\n"));
        const qint64 pid = startWaited();
        QVERIFY(pid > 0);
        QVERIFY(installer.launchHelper(pid));
        QVERIFY(QFileInfo::exists(installer.helperScriptPath()));
        QVERIFY(!QFileInfo::exists(installer.resultPath()));
        // The helper is told the version and where to report.
        const QString script = readFile(installer.helperScriptPath());
        QVERIFY(script.contains(QLatin1String("\nnew_version='0.1.1'\n")));
        QVERIFY(script.contains(QStringLiteral("\nresult_file='%1'\n").arg(installer.resultPath())));
        // Launched once: the helper owns the update now.
        QVERIFY(!installer.launchHelper(pid));

        QVERIFY2(gitbolt::test::waitForLines(installer.logPath(), 2, 10000), qPrintable(diagnostics()));
        QTest::qWait(300);
        QVERIFY(readFile(target).contains(QLatin1String("'0.1.0'")));
        QVERIFY(!QFileInfo::exists(marker()));
        QVERIFY(!QFileInfo::exists(installer.resultPath()));

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
        QCOMPARE(readFile(installer.resultPath()), QStringLiteral("ok 0.1.1 \n"));
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
        QTest::addColumn<QString>("minimumSystem");
        QTest::addColumn<QString>("reason");
        QTest::newRow("another app") << QStringLiteral("wrong-id") << QStringLiteral("0.1.1")
                                     << QStringLiteral("com.example.notgitbolt") << false << QStringLiteral("11.0")
                                     << QStringLiteral("isn't GitBolt: its identifier is com.example.notgitbolt");
        QTest::newRow("another version") << QStringLiteral("wrong-version") << QStringLiteral("0.1.9")
                                         << QStringLiteral("com.gitbolt.app") << false << QStringLiteral("11.0")
                                         << QStringLiteral("is version 0.1.9, not 0.1.1");
        QTest::newRow("broken seal") << QStringLiteral("tampered") << QStringLiteral("0.1.1")
                                     << QStringLiteral("com.gitbolt.app") << true << QStringLiteral("11.0")
                                     << QStringLiteral("fails its signature check");
        // A release built for a newer macOS than this one: once swapped
        // in, LaunchServices wouldn't start it, so it is refused while
        // GitBolt still runs.
        QTest::newRow("needs a newer macOS") << QStringLiteral("newer-macos") << QStringLiteral("0.1.1")
                                             << QStringLiteral("com.gitbolt.app") << false << QStringLiteral("99.0")
                                             << QStringLiteral("GitBolt 0.1.1 needs macOS 99.0 or later, and this "
                                                               "Mac has macOS ");
    }

    void refusesABadDiskImage() {
#if !defined(Q_OS_MACOS)
        QSKIP("macOS only");
#else
        QFETCH(QString, name);
        QFETCH(QString, version);
        QFETCH(QString, id);
        QFETCH(bool, tamper);
        QFETCH(QString, minimumSystem);
        QFETCH(QString, reason);
        const QString dmg = diskImage(name, version, id, tamper, minimumSystem);
        QVERIFY(!dmg.isEmpty());
        const PackageInfo package = installedBundle();
        Installer installer(package, kOld, workDir());
        const Outcome outcome = prepareAndWait(installer, dmg, kNew);
        QVERIFY(!outcome.prepared);
        QVERIFY2(outcome.failed.contains(reason), qPrintable(outcome.failed));
        // Only the minimum macOS would refuse the same download again.
        QCOMPARE(installer.needsNewerSystem(), minimumSystem == QLatin1String("99.0"));
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

    // Cancelled half-way: neither prepared() nor failed() comes, and
    // cancel() returns at once (it runs on the GUI thread, from the
    // dialog's Cancel and Close): it neither waits for the killed step
    // nor detaches nor deletes there. That happens in the background,
    // soon: the copy so far removed, the image detached, its mount
    // point gone.
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
        qint64 cancelMs = -1;
        bool mountedThen = false;
        bool mountedAfter = false;
        const auto cancel = [&] {
            mountedThen = mountedNow();
            QElapsedTimer clock;
            clock.start();
            installer.cancel();
            cancelMs = clock.elapsed();
            // At once, before the event loop: a cancel() that detached
            // the image itself (seconds, with a busy volume) would leave
            // it unmounted here.
            mountedAfter = mountedNow();
        };
        connect(&installer, &Installer::status, this, [&](const QString& text) {
            if (text != step || cancelMs >= 0)
                return;
            if (fromTheSlot)
                cancel();
            else
                QTimer::singleShot(0, &installer, cancel);
        });
        installer.prepare(dmg, kNew);
        QVERIFY(QTest::qWaitFor([&] { return cancelMs >= 0; }, 60000));
        qInfo("cancel() took %lld ms; mounted before it: %d, right after it: %d", cancelMs, mountedThen, mountedAfter);
        // It left the detach to the background, not waited for it: the
        // image is still there when it returns. (That is what makes it
        // quick: hdiutil's detach takes a second or so, more with a busy
        // volume. What cancel() itself costs is mostly startDetached,
        // whose child closes every descriptor up to the soft limit
        // before it execs: ~100 ms at a shell's 1048576, nothing at the
        // 256 a GUI app gets. Hence the loose bound.)
        if (mountedThen)
            QVERIFY2(mountedAfter, "cancel() detached the disk image before it returned");
        QVERIFY2(cancelMs < 1000, qPrintable(QStringLiteral("cancel() took %1 ms").arg(cancelMs)));
        // Copying means mounted: the rows that cancel then must see it.
        if (step == QStringLiteral("Copying GitBolt…"))
            QVERIFY(mountedThen);
        QTest::qWait(500);
        QCOMPARE(prepared.size(), 0);
        QCOMPARE(failed.size(), 0);
        QVERIFY(!installer.busy());
        // The background's part: nothing staged (under any name), nothing
        // mounted, no mount point.
        const auto cleanedUp = [&] {
            return QDir(path(QStringLiteral("Applications")))
                           .entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot)
                       == QStringList{QStringLiteral("GitBolt.app")}
                   && mountPoints().isEmpty() && nothingMountedUnder(workDir());
        };
        QVERIFY2(QTest::qWaitFor(cleanedUp, 30000),
                 qPrintable(QStringLiteral("Applications: %1; mount points: %2 (one when cancelled: %3)")
                                .arg(QDir(path(QStringLiteral("Applications")))
                                         .entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot)
                                         .join(u','),
                                     mountPoints().join(u','), mountedThen ? QStringLiteral("yes") : QStringLiteral("no"))));
        QCOMPARE(copyOf(package.target), QStringLiteral("0.1.0"));
#endif
    }

    // A prepared copy cancelled, then the same version prepared again
    // at once (Install, Cancel, Install): the cancelled copy's removal,
    // still running in the background, never reaches the new one, which
    // has the same name.
    void aCancelledCopysRemovalSparesTheNextOne() {
#if !defined(Q_OS_MACOS)
        QSKIP("macOS only");
#else
        const QString dmg = diskImage(QStringLiteral("good"), kNew.toString(), QStringLiteral("com.gitbolt.app"));
        QVERIFY(!dmg.isEmpty());
        const PackageInfo package = installedBundle();
        Installer installer(package, kOld, workDir());
        Outcome outcome = prepareAndWait(installer, dmg, kNew);
        QVERIFY2(outcome.prepared, qPrintable(outcome.failed));
        const QString staged = path(QStringLiteral("Applications/.GitBolt-0.1.1.app"));
        QVERIFY(QFileInfo(staged).isDir());
        QVERIFY(writeFile(staged + QStringLiteral("/Contents/Resources/first.txt"), "first\n"));
        QElapsedTimer clock;
        clock.start();
        installer.cancel();
        QVERIFY2(clock.elapsed() < 1000, qPrintable(QStringLiteral("cancel() took %1 ms").arg(clock.elapsed())));
        // Out of the way at once, by a rename.
        QVERIFY(!QFileInfo::exists(staged));
        outcome = prepareAndWait(installer, dmg, kNew);
        QVERIFY2(outcome.prepared, qPrintable(outcome.failed));
        // Long enough for the background removal to have finished.
        QVERIFY(QTest::qWaitFor(
            [&] {
                return QDir(path(QStringLiteral("Applications")))
                           .entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot)
                       == (QStringList{QStringLiteral(".GitBolt-0.1.1.app"), QStringLiteral("GitBolt.app")});
            },
            30000));
        QTest::qWait(1000);
        QCOMPARE(copyOf(staged), QStringLiteral("0.1.1"));
        QVERIFY(!QFileInfo::exists(staged + QStringLiteral("/Contents/Resources/first.txt")));
        QString output;
        QVERIFY2(runTool(QStringLiteral("/usr/bin/codesign"),
                         {QStringLiteral("--verify"), QStringLiteral("--deep"), QStringLiteral("--strict"), staged},
                         &output)
                     == 0,
                 qPrintable(output));
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
        QCOMPARE(readFile(installer.resultPath()), QStringLiteral("ok 0.1.1 \n"));
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

    // ---------------------------------------------------------------
    // Leftovers: removeStaleInstallFiles, at startup
    // ---------------------------------------------------------------

    // What a crash, a force quit or a logout left beside the installed
    // bundle: hidden staged copies (cancel()'s renamed ones too) and
    // swapped-aside old copies go. The bundle itself (whatever its
    // name), other apps, near misses, a file of such a name, and a link
    // of any name (and what it points to) stay.
    void removesCopiesLeftBesideTheBundle() {
#if !defined(Q_OS_UNIX)
        QSKIP("links: Unix");
#else
        const PackageInfo package = installedBundle();
        const QString apps = path(QStringLiteral("Applications"));
        for (const QString& name : {QStringLiteral(".GitBolt-0.1.1.app"), QStringLiteral(".GitBolt-0.1.0.app.old"),
                                    QStringLiteral(".GitBolt-0.1.1-cancelled-0a1b2c3d.app")})
            QVERIFY(makeBundle(apps + u'/' + name, kNew.toString()));
        QVERIFY(makeBundle(apps + QStringLiteral("/Other.app"), QStringLiteral("9.9.9"), QStringLiteral("com.example.other")));
        QVERIFY(QDir().mkpath(apps + QStringLiteral("/.GitBolt.app")));
        QVERIFY(QDir().mkpath(apps + QStringLiteral("/.GitBolt-0.1.1.application")));
        QVERIFY(writeFile(apps + QStringLiteral("/.GitBolt-0.1.2.app"), "a file"));
        const QString precious = path(QStringLiteral("precious"));
        QVERIFY(writeFile(precious + QStringLiteral("/keep.txt"), "mine"));
        QVERIFY(QFile::link(precious, apps + QStringLiteral("/.GitBolt-0.1.3.app")));
        QVERIFY(QFile::link(precious, apps + QStringLiteral("/.GitBolt-0.1.4.app.old")));

        removeStaleInstallFiles(package, workDir());
        QStringList left = QDir(apps).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
        left.sort();
        QCOMPARE(left, (QStringList{QStringLiteral(".GitBolt-0.1.1.application"), QStringLiteral(".GitBolt-0.1.2.app"),
                                    QStringLiteral(".GitBolt-0.1.3.app"), QStringLiteral(".GitBolt-0.1.4.app.old"),
                                    QStringLiteral(".GitBolt.app"), QStringLiteral("GitBolt.app"),
                                    QStringLiteral("Other.app")}));
        QCOMPARE(readFile(precious + QStringLiteral("/keep.txt")), QStringLiteral("mine"));
        QCOMPARE(copyOf(package.target), QStringLiteral("0.1.0"));
        QVERIFY(QFileInfo::exists(package.target + QStringLiteral("/Contents/Resources/planted.txt")));

        // A bundle that is itself named like a staged copy stays.
        const QString odd = apps + QStringLiteral("/.GitBolt-0.1.0.app");
        QVERIFY(makeBundle(odd, kOld.toString()));
        removeStaleInstallFiles({PackageKind::MacBundle, odd, true, {}}, workDir());
        QCOMPARE(copyOf(odd), QStringLiteral("0.1.0"));
#endif
    }

    // The disk image mount points prepare() makes in workDir: an empty
    // one goes; one with something in it (so not ours to empty) and a
    // link of the name stay, and so does everything else there.
    void removesMountPointsLeftInTheWorkDir() {
        QVERIFY(QDir().mkpath(workDir() + QStringLiteral("/mount-AbC123")));
        QVERIFY(writeFile(workDir() + QStringLiteral("/mount-XyZ789/keep.txt"), "x"));
        QVERIFY(writeFile(workDir() + QStringLiteral("/install.log"), "log"));
        QVERIFY(writeFile(workDir() + QStringLiteral("/install-result"), "ok 0.1.1 \n"));
        QVERIFY(!download(PackageKind::Deb).isEmpty());
        QStringList expected{QStringLiteral("0.1.1"), QStringLiteral("install-result"), QStringLiteral("install.log"),
                             QStringLiteral("mount-XyZ789")};
#if defined(Q_OS_UNIX)
        const QString elsewhere = path(QStringLiteral("elsewhere"));
        QVERIFY(QDir().mkpath(elsewhere));
        QVERIFY(QFile::link(elsewhere, workDir() + QStringLiteral("/mount-link")));
        expected << QStringLiteral("mount-link");
#endif
        removeStaleInstallFiles(PackageInfo{}, workDir());
        QStringList left = QDir(workDir()).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
        left.sort();
        QCOMPARE(left, expected);
        QVERIFY(QFileInfo::exists(workDir() + QStringLiteral("/mount-XyZ789/keep.txt")));
        // Nothing for a work folder that isn't there, or a relative one.
        removeStaleInstallFiles(PackageInfo{}, path(QStringLiteral("missing")));
        removeStaleInstallFiles(PackageInfo{}, QStringLiteral("relative"));
    }

    // macOS: a disk image still attached at a mount point of ours (the
    // process died while it was mounted) is detached, and the mount
    // point removed. The image itself stays where it was.
    void detachesADiskImageLeftMounted() {
#if !defined(Q_OS_MACOS)
        QSKIP("macOS only");
#else
        const QString dmg = diskImage(QStringLiteral("good"), kNew.toString(), QStringLiteral("com.gitbolt.app"));
        QVERIFY(!dmg.isEmpty());
        const QString mount = workDir() + QStringLiteral("/mount-Left01");
        QVERIFY(QDir().mkpath(mount));
        QString output;
        int status = -1;
        for (int attempt = 1; attempt <= 5 && status != 0; ++attempt) {
            if (attempt > 1)
                QTest::qWait(attempt * 1000);
            status = runTool(QStringLiteral("/usr/bin/hdiutil"),
                             {QStringLiteral("attach"), QStringLiteral("-readonly"), QStringLiteral("-nobrowse"),
                              QStringLiteral("-noautoopen"), QStringLiteral("-mountpoint"), mount, dmg},
                             &output);
        }
        QVERIFY2(status == 0, qPrintable(output));
        auto detach = qScopeGuard([&] {
            if (QFileInfo::exists(mount + QStringLiteral("/GitBolt.app")))
                runTool(QStringLiteral("/usr/bin/hdiutil"), {QStringLiteral("detach"), QStringLiteral("-force"), mount});
        });
        QVERIFY(QFileInfo::exists(mount + QStringLiteral("/GitBolt.app")));

        removeStaleInstallFiles(installedBundle(), workDir());
        QVERIFY(!QFileInfo::exists(mount));
        QVERIFY(mountPoints().isEmpty());
        QVERIFY(nothingMountedUnder(workDir()));
        QVERIFY(QFileInfo(dmg).isFile());
#endif
    }

    // Beside the AppImage: the Downloader's .part files (of any
    // version) go, and so does the one checked download GitBolt
    // recorded and no helper took over (a crash's). Every other
    // GitBolt-<v>-x86_64.AppImage stays, whatever its version: that is
    // the name users keep their AppImages under, an older one kept to
    // go back to, or their main copy, which an update leaves under the
    // old version's name. So do names that aren't the release's, other
    // files, folders and links, and the running AppImage.
    void removesOnlyTheAppImageDownloadsGitBoltLeft() {
        const QString apps = path(QStringLiteral("My Apps"));
        const QString target = apps + QStringLiteral("/GitBolt.AppImage");
        QVERIFY(writeFile(target, fakeAppScript(QStringLiteral("0.1.1")), true));
        for (const QString& name :
             {QStringLiteral(".GitBolt-0.1.1-x86_64.AppImage.part"), QStringLiteral(".GitBolt-0.1.2-x86_64.AppImage.part"),
              QStringLiteral("GitBolt-0.0.9-x86_64.AppImage"), QStringLiteral("GitBolt-0.1.0-x86_64.AppImage"),
              QStringLiteral("GitBolt-0.1.1-x86_64.AppImage"), QStringLiteral("GitBolt-0.1.10-x86_64.AppImage"),
              QStringLiteral("GitBolt-01.1.0-x86_64.AppImage"), QStringLiteral("GitBolt-0.1.0-x86_64.AppImage.zsync"),
              QStringLiteral("GitBolt-0.1.0-aarch64.AppImage"), QStringLiteral("notes.txt")})
            QVERIFY(writeFile(apps + u'/' + name, "x"));
        // What a GitBolt that crashed after its download left: recorded.
        const QString download = apps + QStringLiteral("/GitBolt-0.1.2-x86_64.AppImage");
        QVERIFY(writeFile(download, "checked download"));
        recordPendingDownload(workDir(), download);
        QVERIFY(QDir().mkpath(apps + QStringLiteral("/GitBolt-0.0.7-x86_64.AppImage")));
        QStringList expected{QStringLiteral("GitBolt-0.0.7-x86_64.AppImage"), QStringLiteral("GitBolt-0.0.9-x86_64.AppImage"),
                             QStringLiteral("GitBolt-0.1.0-aarch64.AppImage"), QStringLiteral("GitBolt-0.1.0-x86_64.AppImage"),
                             QStringLiteral("GitBolt-0.1.0-x86_64.AppImage.zsync"),
                             QStringLiteral("GitBolt-0.1.1-x86_64.AppImage"), QStringLiteral("GitBolt-0.1.10-x86_64.AppImage"),
                             QStringLiteral("GitBolt-01.1.0-x86_64.AppImage"), QStringLiteral("GitBolt.AppImage"),
                             QStringLiteral("notes.txt")};
#if defined(Q_OS_UNIX)
        const QString precious = path(QStringLiteral("precious.AppImage"));
        QVERIFY(writeFile(precious, "mine"));
        QVERIFY(QFile::link(precious, apps + QStringLiteral("/GitBolt-0.0.8-x86_64.AppImage")));
        QVERIFY(QFile::link(precious, apps + QStringLiteral("/.GitBolt-0.0.8-x86_64.AppImage.part")));
        expected << QStringLiteral(".GitBolt-0.0.8-x86_64.AppImage.part") << QStringLiteral("GitBolt-0.0.8-x86_64.AppImage");
#endif
        expected.sort();

        removeStaleInstallFiles({PackageKind::AppImage, target, true, {}}, workDir());
        QStringList left = QDir(apps).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
        left.sort();
        QCOMPARE(left, expected);
        QCOMPARE(readFile(apps + QStringLiteral("/GitBolt-0.0.9-x86_64.AppImage")), QStringLiteral("x"));
        QVERIFY(!QFileInfo::exists(workDir() + u'/' + QLatin1String(kPendingDownloadFile)));
#if defined(Q_OS_UNIX)
        QCOMPARE(readFile(precious), QStringLiteral("mine"));
#endif

        // The running AppImage named as the release names it: never
        // removed. Nor does any other package's cleanup touch this
        // folder.
        const QString named = apps + QStringLiteral("/GitBolt-0.1.1-x86_64.AppImage");
        QVERIFY(writeFile(named, fakeAppScript(QStringLiteral("0.1.1")), true));
        removeStaleInstallFiles({PackageKind::AppImage, named, true, {}}, workDir());
        QVERIFY(QFileInfo(named).isFile());
        QVERIFY(writeFile(apps + QStringLiteral("/.GitBolt-0.1.3-x86_64.AppImage.part"), "x"));
        removeStaleInstallFiles({PackageKind::Deb, apps, true, {}}, workDir());
        removeStaleInstallFiles({PackageKind::WindowsInstall, apps, true, {}}, workDir());
        QVERIFY(QFileInfo::exists(apps + QStringLiteral("/.GitBolt-0.1.3-x86_64.AppImage.part")));
        QVERIFY(QFileInfo::exists(apps + QStringLiteral("/GitBolt-0.1.0-x86_64.AppImage")));
    }

    // The record of a download (recordPendingDownload): the file it
    // names goes at the next start only while it is still the download
    // GitBolt left, and the record goes whatever it names.
    void aRecordedDownloadGoesOnlyWhileItIsGitBolts_data() {
        QTest::addColumn<QString>("what");
        QTest::addColumn<bool>("removed");
        QTest::newRow("the download") << QStringLiteral("download") << true;
        QTest::newRow("gone already") << QStringLiteral("missing") << false;
        QTest::newRow("the running AppImage") << QStringLiteral("target") << false;
        QTest::newRow("a name not the release's") << QStringLiteral("other name") << false;
        QTest::newRow("a folder") << QStringLiteral("folder") << false;
        QTest::newRow("a link") << QStringLiteral("link") << false;
        QTest::newRow("a relative path") << QStringLiteral("relative") << false;
        QTest::newRow("too long to be a record") << QStringLiteral("too long") << false;
        QTest::newRow("two lines") << QStringLiteral("two lines") << false;
        QTest::newRow("another package starts") << QStringLiteral("deb") << false;
    }
    void aRecordedDownloadGoesOnlyWhileItIsGitBolts() {
        QFETCH(QString, what);
        QFETCH(bool, removed);
#if !defined(Q_OS_UNIX)
        if (what == QLatin1String("link"))
            QSKIP("QFile::link makes a .lnk shortcut on Windows");
#endif
        const QString apps = path(QStringLiteral("My Apps"));
        const QString download = apps + QStringLiteral("/GitBolt-0.1.2-x86_64.AppImage");
        const QString precious = path(QStringLiteral("precious.AppImage"));
        QVERIFY(writeFile(precious, "mine"));
        QString target = apps + QStringLiteral("/GitBolt.AppImage");
        QString recorded = download;
        if (what == QLatin1String("target")) {
            target = download;
            QVERIFY(writeFile(download, fakeAppScript(QStringLiteral("0.1.2")), true));
        } else if (what == QLatin1String("other name")) {
            recorded = apps + QStringLiteral("/GitBolt-0.1.2-aarch64.AppImage");
            QVERIFY(writeFile(recorded, "x"));
        } else if (what == QLatin1String("folder")) {
            QVERIFY(QDir().mkpath(download));
        } else if (what == QLatin1String("link")) {
            QVERIFY(QDir().mkpath(apps));
            QVERIFY(QFile::link(precious, download));
        } else if (what != QLatin1String("missing")) {
            QVERIFY(writeFile(download, "checked download"));
        }
        if (target != download)
            QVERIFY(writeFile(target, fakeAppScript(QStringLiteral("0.1.1")), true));
        recordPendingDownload(workDir(), recorded);
        const QString record = workDir() + u'/' + QLatin1String(kPendingDownloadFile);
        QCOMPARE(readFile(record), recorded + u'\n');
        if (what == QLatin1String("relative"))
            QVERIFY(writeFile(record, "My Apps/GitBolt-0.1.2-x86_64.AppImage\n"));
        else if (what == QLatin1String("too long"))
            QVERIFY(writeFile(record, (download + u'\n').toUtf8() + QByteArray(70 * 1024, ' ')));
        else if (what == QLatin1String("two lines"))
            QVERIFY(writeFile(record, (download + u'\n' + download + u'\n').toUtf8()));

        const PackageInfo package = what == QLatin1String("deb")
                                        ? PackageInfo{PackageKind::Deb, QStringLiteral("/usr/lib/gitbolt"), true, {}}
                                        : PackageInfo{PackageKind::AppImage, target, true, {}};
        removeStaleInstallFiles(package, workDir());
        QVERIFY(!QFileInfo::exists(record) && !QFileInfo(record).isSymLink());
        QCOMPARE(QFileInfo::exists(recorded) || QFileInfo(recorded).isSymLink(),
                 !removed && what != QLatin1String("missing"));
        QCOMPARE(readFile(precious), QStringLiteral("mine"));
        QVERIFY(QFileInfo(target).isFile());
    }

    // The record is a file of its own, made anew: a link planted at its
    // name is replaced, never written through; forgetting it removes it.
    void recordsAndForgetsADownload() {
        const QString record = workDir() + u'/' + QLatin1String(kPendingDownloadFile);
        const QString download = path(QStringLiteral("My Apps/GitBolt-0.1.2-x86_64.AppImage"));
        recordPendingDownload(workDir(), download);
        QCOMPARE(readFile(record), download + u'\n');
        forgetPendingDownload(workDir());
        QVERIFY(!QFileInfo::exists(record));
        forgetPendingDownload(workDir());   // nothing to forget: fine
#if defined(Q_OS_UNIX)
        const QString precious = path(QStringLiteral("precious.txt"));
        QVERIFY(writeFile(precious, "mine"));
        QVERIFY(QFile::link(precious, record));
        recordPendingDownload(workDir(), download);
        QVERIFY(!QFileInfo(record).isSymLink());
        QCOMPARE(readFile(record), download + u'\n');
        QCOMPARE(readFile(precious), QStringLiteral("mine"));
#endif
        // No folder to record in: nothing written anywhere.
        recordPendingDownload(QStringLiteral("relative"), download);
        QVERIFY(!QFileInfo::exists(QStringLiteral("relative")));
    }
};

QTEST_GUILESS_MAIN(TestInstaller)
#include "TestInstaller.moc"
