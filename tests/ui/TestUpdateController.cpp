//
// TestUpdateController — in-app updates on the window's side
// (ui/UpdateController.h), against the loopback stub
// (../update/HttpServer.h), never GitHub:
//
//   - a manual check shows the dialog with whatever it finds: newer
//     (installable or not, and why), the same or older, a failure,
//     which Try Again asks again; every answer is saved as lastCheck,
//     a failure isn't;
//   - the automatic check (its timer fired, as the e2e tests do) shows
//     the notice bar, asks again only once a day, and says nothing about
//     a skipped version, a version whose bar was dismissed this session,
//     or anything with the switch off or GITBOLT_UPDATE_CHECK=0;
//   - only an official build's setup schedules the timer; a test
//     session's exists, stopped, and may be fired; start() clears old
//     downloads;
//   - Install and Restart downloads into the AppImage's folder; Cancel,
//     closing the dialog, a failed download and a download abandoned
//     after it finished all leave nothing there but the running AppImage,
//     and never start the helper; on macOS, a disk image that won't
//     attach is deleted too;
//   - what Install and Skip act on is the release the dialog shows: a
//     newer one found meanwhile waits for the bar, and a manual check
//     whose dialog was taken over by the bar's View doesn't replace the
//     install screen with its late answer;
//   - once the helper runs (a real one, against a stand-in AppImage
//     and a child process for it to wait for; the quit is caught, never
//     done), the dialog has nothing to cancel, says so when GitBolt
//     doesn't quit, and the download is the helper's;
//   - after an install, start() reads the helper's install-result
//     once: ok goes to the status bar; declined, failed or gave-up for
//     a newer version open the dialog on why, the end of install.log
//     and how to install by hand for this kind of package; a stale or
//     unreadable one is dropped;
//   - MainWindow: without the in-app check (a source build, or a test
//     session with no test feed) Help › Check for Updates opens the
//     releases page; with it, it asks the feed.
//

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QScopeGuard>
#include <QSettings>
#include <QSignalSpy>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QVBoxLayout>

#include "../update/HttpServer.h"
#include "conf/SettingsService.h"
#include "dialogs/UpdateDialog.h"
#include "ui/MainWindow.h"
#include "ui/UpdateController.h"
#include "ui/UpdateSetup.h"
#include "update/Checker.h"
#include "update/Downloader.h"
#include "update/Package.h"
#include "widgets/UpdateNoticeBar.h"

using gitbolt::conf::SettingsService;
using gitbolt::dialogs::UpdateDialog;
using gitbolt::test::HttpServer;
using gitbolt::test::Served;
using gitbolt::ui::UpdateController;
using gitbolt::ui::UpdateGate;
using gitbolt::ui::UpdateSetup;
using gitbolt::update::PackageInfo;
using gitbolt::update::PackageKind;
using gitbolt::widgets::UpdateNoticeBar;
using State = UpdateDialog::State;

namespace {

const QByteArray kFeedPath = "/repos/Inverian/GitBolt/releases/latest";
const QString kAppImageAsset = QStringLiteral("GitBolt-0.1.1-x86_64.AppImage");

class UrlCatcher : public QObject {
    Q_OBJECT
public:
    UrlCatcher() {
        QDesktopServices::setUrlHandler(QStringLiteral("http"), this, "open");
        QDesktopServices::setUrlHandler(QStringLiteral("https"), this, "open");
    }
    ~UrlCatcher() override {
        QDesktopServices::unsetUrlHandler(QStringLiteral("http"));
        QDesktopServices::unsetUrlHandler(QStringLiteral("https"));
    }
    QList<QUrl> urls;
public slots:
    void open(const QUrl& url) { urls << url; }
};

// A release as GitHub's releases/latest has it, its assets on `server`,
// with SHA256SUMS.txt over them unless `checksums` is false. `sums`
// overrides what that file says.
QByteArray releaseJson(const HttpServer& server, const QString& version,
                       const QList<QPair<QString, QByteArray>>& assets, bool checksums = true,
                       const QByteArray& sums = {}) {
    QJsonArray list;
    QByteArray sumsFile;
    const auto add = [&](const QString& name, const QByteArray& body) {
        list.append(QJsonObject{
            {QStringLiteral("name"), name},
            {QStringLiteral("browser_download_url"),
             server.url(QStringLiteral("/download/v%1/%2").arg(version, name)).toString()},
            {QStringLiteral("size"), body.size()},
        });
    };
    for (const auto& [name, body] : assets) {
        add(name, body);
        sumsFile += QCryptographicHash::hash(body, QCryptographicHash::Sha256).toHex() + "  " + name.toUtf8() + '\n';
    }
    if (checksums)
        add(QStringLiteral("SHA256SUMS.txt"), sums.isEmpty() ? sumsFile : sums);
    return QJsonDocument(QJsonObject{
                             {QStringLiteral("tag_name"), QStringLiteral("v") + version},
                             {QStringLiteral("name"), QStringLiteral("GitBolt ") + version},
                             {QStringLiteral("draft"), false},
                             {QStringLiteral("prerelease"), false},
                             {QStringLiteral("html_url"),
                              QStringLiteral("https://github.com/Inverian/GitBolt/releases/tag/v") + version},
                             {QStringLiteral("body"), QStringLiteral("## [%1]\n\n- Fixed.").arg(version)},
                             {QStringLiteral("assets"), list},
                         })
        .toJson();
}

// Serves `version`'s release on the feed, and its files.
void serve(HttpServer& server, const QString& version, const QList<QPair<QString, QByteArray>>& assets,
           bool checksums = true, const QByteArray& sums = {}) {
    server.routes.insert(kFeedPath, Served::json(releaseJson(server, version, assets, checksums, sums)));
    QByteArray sumsFile;
    for (const auto& [name, body] : assets) {
        server.routes.insert(QStringLiteral("/download/v%1/%2").arg(version, name).toUtf8(), Served::ok(body));
        sumsFile += QCryptographicHash::hash(body, QCryptographicHash::Sha256).toHex() + "  " + name.toUtf8() + '\n';
    }
    server.routes.insert(QStringLiteral("/download/v%1/SHA256SUMS.txt").arg(version).toUtf8(),
                         Served::ok(sums.isEmpty() ? sumsFile : sums));
}

// main()'s setup for a test session pointed at `server`, running 0.1.0.
UpdateSetup testSetup(const HttpServer& server, const QString& cacheDir, PackageInfo package = {}) {
    gitbolt::ui::UpdateSetupInputs inputs;
    inputs.testSession = gitbolt::util::TestSessionKind::Bridge;
    inputs.env.insert(QStringLiteral("GITBOLT_TEST_UPDATE_FEED"), server.url(QString::fromLatin1(kFeedPath)).toString());
    inputs.env.insert(QStringLiteral("GITBOLT_TEST_APP_VERSION"), QStringLiteral("0.1.0"));
    inputs.appVersion = QStringLiteral("0.1.5");
    inputs.cacheDir = cacheDir;
    // A test session's downloads: the temporary folder's, never the cache.
    inputs.tempDir = cacheDir;
    UpdateSetup setup = gitbolt::ui::setUpUpdates(inputs);
    setup.package = std::move(package);
    return setup;
}

QStringList entries(const QString& dir) {
    return QDir(dir).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name);
}

QByteArray contents(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray("<missing>");
}

bool writeFile(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size();
}

// The buttons the dialog shows, by objectName.
QStringList shownButtons(const UpdateDialog* dialog) {
    QStringList names;
    for (const QPushButton* b : dialog->findChildren<QPushButton*>()) {
        if (!b->isHidden())
            names << b->objectName();
    }
    names.sort();
    return names;
}

QString headline(const UpdateDialog* dialog) {
    return dialog->findChild<QLabel*>(QStringLiteral("updateDialog.headline"))->text();
}

// Every package's installer, so that any kind can install 0.1.1.
const QList<QPair<QString, QByteArray>> kEveryInstaller{
    {QStringLiteral("GitBolt-0.1.1-Darwin-arm64.dmg"), "dmg"},
    {QStringLiteral("GitBolt-0.1.1-Windows-AMD64.exe"), "exe"},
    {QStringLiteral("gitbolt_0.1.1_amd64.deb"), "deb"},
    {QStringLiteral("GitBolt-0.1.1-x86_64.AppImage"), "AppImage"},
};

// The window's side of it, without a MainWindow: the controller with a
// window, the window's settings and its notice bar.
struct Harness {
    explicit Harness(UpdateSetup setup) {
        auto* column = new QVBoxLayout(&window);
        bar = new UpdateNoticeBar(&window);
        column->addWidget(bar);
        controller = new UpdateController(std::move(setup), &settings, bar, &window);
    }

    UpdateDialog* dialog() const { return controller->dialog(); }
    QTimer* timer() const { return window.findChild<QTimer*>(QStringLiteral("updateCheckTimer")); }
    gitbolt::update::Downloader* downloader() const { return window.findChild<gitbolt::update::Downloader*>(); }
    gitbolt::update::Checker* checker() const { return window.findChild<gitbolt::update::Checker*>(); }
    // As the bridge's fire-timer does.
    void fire() const { QMetaObject::invokeMethod(timer(), "timeout"); }
    QPushButton* button(const char* name) const {
        return dialog()->findChild<QPushButton*>(QString::fromLatin1(name));
    }

    QWidget window;
    SettingsService settings;
    UpdateNoticeBar* bar = nullptr;
    UpdateController* controller = nullptr;
};

} // namespace

class TestUpdateController : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        QVERIFY(settingsDir_.isValid());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir_.path());
    }

    // A fresh GitBolt.ini and cache for each test.
    void init() {
        QSettings(SettingsService().fileName(), QSettings::IniFormat).clear();
        cache_ = std::make_unique<QTemporaryDir>();
        QVERIFY(cache_->isValid());
    }

    // ---- checking ---------------------------------------------------------

    void manualCheckShowsWhatItFinds_data() {
        QTest::addColumn<QString>("served");
        QTest::addColumn<int>("state");
        QTest::addColumn<QString>("says");
        QTest::newRow("newer") << "0.1.1" << int(State::NotInstallable)
                               << "wasn't installed from a GitBolt release package";
        QTest::newRow("same") << "0.1.0" << int(State::UpToDate) << "GitBolt 0.1.0 is the latest version.";
        QTest::newRow("older") << "0.0.9" << int(State::UpToDate) << "This copy, 0.1.0, is newer.";
        QTest::newRow("error") << "" << int(State::Failed) << "HTTP 500";
    }
    void manualCheckShowsWhatItFinds() {
        QFETCH(QString, served);
        QFETCH(int, state);
        QFETCH(QString, says);
        HttpServer server;
        if (served.isEmpty())
            server.routes.insert(kFeedPath, Served::withStatus(500, "{}"));
        else
            serve(server, served, {{kAppImageAsset, "new"}});
        // The test binary is no package: Unknown, as a build tree is.
        Harness h(testSetup(server, cache_->path(), gitbolt::update::detectPackage(gitbolt::update::currentProbe())));

        h.controller->checkNow();
        QVERIFY(h.dialog() && h.dialog()->isVisible());
        QCOMPARE(h.dialog()->state(), State::Checking);
        QTRY_COMPARE(int(h.dialog()->state()), state);
        QVERIFY2(h.dialog()->shownText().contains(says), qPrintable(h.dialog()->shownText()));
        QCOMPARE(server.hits(kFeedPath), 1);
        // An answer is saved, a failure isn't; the bar stays out of it.
        QCOMPARE(h.settings.updateLastCheck().isValid(), !served.isEmpty());
        QVERIFY(h.bar->isHidden());
    }

    void tryAgainAsksAgain() {
        HttpServer server;
        server.routes.insert(kFeedPath, Served::withStatus(500, "{}"));
        Harness h(testSetup(server, cache_->path()));
        h.controller->checkNow();
        QTRY_COMPARE(h.dialog()->state(), State::Failed);

        serve(server, QStringLiteral("0.1.0"), {});
        h.button("updateDialog.retry")->click();
        QTRY_COMPARE(h.dialog()->state(), State::UpToDate);
        QCOMPARE(server.hits(kFeedPath), 2);
    }

    // A manual check while the dialog is closed mid-way: what it finds
    // goes to the bar.
    void closingWhileCheckingLeavesTheFindToTheBar() {
        HttpServer server;
        serve(server, QStringLiteral("0.1.1"), {{kAppImageAsset, "new"}});
        server.routes[kFeedPath].delayMs = 300;
        Harness h(testSetup(server, cache_->path()));
        h.controller->checkNow();
        h.dialog()->reject();
        QTRY_COMPARE(h.bar->isHidden(), false);
        QCOMPARE(h.bar->text(), QStringLiteral("GitBolt 0.1.1 is available."));
        QVERIFY(!h.dialog()->isVisible());
    }

    // ---- the automatic check ------------------------------------------------

    void automaticFindShowsTheBarOnceADay() {
        HttpServer server;
        serve(server, QStringLiteral("0.1.1"), {{kAppImageAsset, "new"}});
        Harness h(testSetup(server, cache_->path()));

        h.fire();
        QTRY_COMPARE(h.bar->isHidden(), false);
        QCOMPARE(h.bar->text(), QStringLiteral("GitBolt 0.1.1 is available."));
        QVERIFY(!h.dialog());   // no dialog for an automatic find
        QVERIFY(h.settings.updateLastCheck().isValid());

        // Not due again for a day.
        h.fire();
        QTest::qWait(200);
        QCOMPARE(server.hits(kFeedPath), 1);

        // View opens the dialog on what it found, without asking again.
        h.bar->findChild<QPushButton*>(QStringLiteral("updateNotice.view"))->click();
        QVERIFY(h.bar->isHidden());
        QVERIFY(h.dialog() && h.dialog()->isVisible());
        QCOMPARE(h.dialog()->state(), State::NotInstallable);
        QCOMPARE(server.hits(kFeedPath), 1);
    }

    void skippedVersionIsQuietUntilANewerOne() {
        HttpServer server;
        serve(server, QStringLiteral("0.1.1"), {{kAppImageAsset, "new"}});
        Harness h(testSetup(server, cache_->path()));
        h.settings.setSkippedUpdateVersion(QStringLiteral("0.1.1"));

        h.fire();
        QTRY_COMPARE(server.hits(kFeedPath), 1);
        QTest::qWait(200);
        QVERIFY(h.bar->isHidden());

        // A manual check shows it all the same.
        h.controller->checkNow();
        QTRY_COMPARE(h.dialog()->state(), State::NotInstallable);
        h.dialog()->reject();

        // A newer one is news again.
        serve(server, QStringLiteral("0.1.2"), {{QStringLiteral("GitBolt-0.1.2-x86_64.AppImage"), "new"}});
        h.settings.setUpdateLastCheck({});
        h.fire();
        QTRY_COMPARE(h.bar->isHidden(), false);
        QCOMPARE(h.bar->version(), QStringLiteral("0.1.2"));
    }

    void skipThisVersionIsSaved() {
        HttpServer server;
        serve(server, QStringLiteral("0.1.1"), {{kAppImageAsset, "new"}});
        Harness h(testSetup(server, cache_->path()));
        h.controller->checkNow();
        QTRY_COMPARE(h.dialog()->state(), State::NotInstallable);
        h.button("updateDialog.skip")->click();
        QCOMPARE(h.settings.skippedUpdateVersion(), QStringLiteral("0.1.1"));
        QVERIFY(!h.dialog()->isVisible());
    }

    void dismissingLastsTheSession() {
        HttpServer server;
        serve(server, QStringLiteral("0.1.1"), {{kAppImageAsset, "new"}});
        {
            Harness h(testSetup(server, cache_->path()));
            h.fire();
            QTRY_COMPARE(h.bar->isHidden(), false);
            h.bar->findChild<QAbstractButton*>(QStringLiteral("updateNotice.dismiss"))->click();
            QVERIFY(h.bar->isHidden());

            h.settings.setUpdateLastCheck({});
            h.fire();
            QTRY_COMPARE(server.hits(kFeedPath), 2);
            QTest::qWait(200);
            QVERIFY(h.bar->isHidden());
            QVERIFY(h.settings.skippedUpdateVersion().isEmpty());
        }
        // The next session says it again.
        Harness again(testSetup(server, cache_->path()));
        again.settings.setUpdateLastCheck({});
        again.fire();
        QTRY_COMPARE(again.bar->isHidden(), false);
    }

    void switchOffOrOptOutAsksNothingByItself() {
        HttpServer server;
        serve(server, QStringLiteral("0.1.1"), {{kAppImageAsset, "new"}});
        {
            Harness h(testSetup(server, cache_->path()));
            h.settings.setUpdateAutoCheck(false);
            h.fire();
            QTest::qWait(300);
            QCOMPARE(server.requests().size(), 0);
            // Asked, it asks.
            h.controller->checkNow();
            QTRY_COMPARE(server.hits(kFeedPath), 1);
            h.settings.setUpdateAutoCheck(true);
        }
        // GITBOLT_UPDATE_CHECK=0, the switch on and the check due: the
        // timer, fired, asks nothing either.
        UpdateSetup optedOut = testSetup(server, cache_->path());
        optedOut.envOptOut = true;
        Harness h(optedOut);
        h.settings.setUpdateLastCheck({});
        h.fire();
        QTest::qWait(300);
        QCOMPARE(server.hits(kFeedPath), 1);
    }

    // ---- scheduling ---------------------------------------------------------

    void onlyAnOfficialBuildSchedules() {
        HttpServer server;
        {
            Harness test(testSetup(server, cache_->path()));
            test.controller->start();
            QVERIFY(!test.timer()->isActive());
            QVERIFY(test.timer()->property("fireWhileStopped").toBool());
        }
        UpdateSetup official = testSetup(server, cache_->path());
        official.gate = UpdateGate::Active;
        {
            Harness h(official);
            QVERIFY(!h.timer()->property("fireWhileStopped").toBool());
            h.controller->start();
            QVERIFY(h.timer()->isActive());
            QCOMPARE(h.timer()->interval(), UpdateController::kFirstCheckDelayMs);
            // The first tick, then hourly. Not due: nothing asked.
            h.settings.setUpdateLastCheck(QDateTime::currentDateTimeUtc());
            h.fire();
            QCOMPARE(h.timer()->interval(), UpdateController::kCheckIntervalMs);
            QVERIFY(h.timer()->isActive());
            QTest::qWait(200);
            QCOMPARE(server.requests().size(), 0);
        }
        official.envOptOut = true;
        Harness optedOut(official);
        optedOut.controller->start();
        QVERIFY(!optedOut.timer()->isActive());
    }

    void startClearsOldDownloads() {
        HttpServer server;
        const UpdateSetup setup = testSetup(server, cache_->path());
        const QString updates = setup.config.downloadDir;
        for (const char* version : {"0.0.9", "0.1.0", "0.1.1"})
            QVERIFY(QDir().mkpath(QDir(updates).filePath(QString::fromLatin1(version))));
        Harness h(setup);
        h.controller->start();
        QCOMPARE(entries(updates), QStringList{QStringLiteral("0.1.1")});
    }

    // What an update that crashed left beside the AppImage goes at the
    // next start (removeStaleInstallFiles); the running copy and a newer
    // release's AppImage stay.
    void startClearsWhatACrashedUpdateLeftBehind() {
        HttpServer server;
        const QString apps = appImageFolder();
        for (const char* name : {".GitBolt-0.1.1-x86_64.AppImage.part", "GitBolt-0.0.9-x86_64.AppImage",
                                 "GitBolt-0.1.2-x86_64.AppImage"}) {
            QFile file(QDir(apps).filePath(QString::fromLatin1(name)));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("x");
        }
        Harness h(testSetup(server, cache_->path(), appImage(apps)));
        h.controller->start();
        QCOMPARE(entries(apps), (QStringList{QStringLiteral("GitBolt-0.1.2-x86_64.AppImage"),
                                             QStringLiteral("GitBolt.AppImage")}));
    }

    // ---- installing ---------------------------------------------------------

    void cancelLeavesNothingBehind() {
        HttpServer server;
        serve(server, QStringLiteral("0.1.1"), {{kAppImageAsset, QByteArray(256 * 1024, 'n')}});
        auto& slow = server.routes[QStringLiteral("/download/v0.1.1/%1").arg(kAppImageAsset).toUtf8()];
        slow.chunkBytes = 4096;
        slow.chunkDelayMs = 40;
        const QString apps = appImageFolder();
        Harness h(testSetup(server, cache_->path(), appImage(apps)));
        h.controller->checkNow();
        QTRY_COMPARE(h.dialog()->state(), State::Available);

        h.button("updateDialog.install")->click();
        QCOMPARE(h.dialog()->state(), State::Downloading);
        // Into the AppImage's folder, as a .part, until Cancel.
        QTRY_VERIFY(entries(apps).contains(QStringLiteral(".") + kAppImageAsset + QStringLiteral(".part")));
        h.button("updateDialog.cancel")->click();
        QCOMPARE(h.dialog()->state(), State::Available);
        QVERIFY(h.dialog()->isVisible());
        QCOMPARE(entries(apps), QStringList{QStringLiteral("GitBolt.AppImage")});
        QCOMPARE(contents(QDir(apps).filePath(QStringLiteral("GitBolt.AppImage"))), QByteArray("old"));
    }

    void closingTheDialogStopsTheDownload() {
        HttpServer server;
        serve(server, QStringLiteral("0.1.1"), {{kAppImageAsset, QByteArray(256 * 1024, 'n')}});
        auto& slow = server.routes[QStringLiteral("/download/v0.1.1/%1").arg(kAppImageAsset).toUtf8()];
        slow.chunkBytes = 4096;
        slow.chunkDelayMs = 40;
        const QString apps = appImageFolder();
        Harness h(testSetup(server, cache_->path(), appImage(apps)));
        h.controller->checkNow();
        QTRY_COMPARE(h.dialog()->state(), State::Available);
        h.button("updateDialog.install")->click();
        QTRY_VERIFY(entries(apps).size() == 2);
        h.dialog()->reject();
        QVERIFY(!h.downloader()->busy());
        QCOMPARE(entries(apps), QStringList{QStringLiteral("GitBolt.AppImage")});
    }

    // The download finished and was checked, and then the user walked
    // away (closed the dialog) before it was installed: the new AppImage
    // doesn't stay next to the old one, and no helper is started.
    void anAbandonedDownloadIsDeleted() {
        HttpServer server;
        serve(server, QStringLiteral("0.1.1"), {{kAppImageAsset, "the new AppImage"}});
        const QString apps = appImageFolder();
        Harness h(testSetup(server, cache_->path(), appImage(apps)));
        h.controller->checkNow();
        QTRY_COMPARE(h.dialog()->state(), State::Available);

        // Right after the controller takes the download (it connected
        // first), before the Installer's queued outcome.
        bool abandoned = false;
        connect(h.downloader(), &gitbolt::update::Downloader::finished, this,
                [&](const QString& path) {
                    QCOMPARE(path, QDir(apps).filePath(kAppImageAsset));
                    QVERIFY(QFile::exists(path));
                    h.dialog()->reject();
                    abandoned = true;
                });
        h.button("updateDialog.install")->click();
        QTRY_VERIFY(abandoned);
        QTest::qWait(300);   // anything queued has run
        QCOMPARE(entries(apps), QStringList{QStringLiteral("GitBolt.AppImage")});
        QCOMPARE(contents(QDir(apps).filePath(QStringLiteral("GitBolt.AppImage"))), QByteArray("old"));
        const QString updates = h.controller->setup().config.downloadDir;
        QVERIFY(!QFile::exists(QDir(updates).filePath(QStringLiteral("install-helper.sh"))));
        QVERIFY(!QFile::exists(QDir(updates).filePath(QStringLiteral("install.log"))));
    }

    void aBadDownloadFailsCleanlyAndCanBeRetried() {
        HttpServer server;
        // SHA256SUMS.txt says something else.
        serve(server, QStringLiteral("0.1.1"), {{kAppImageAsset, "the new AppImage"}}, true,
              QByteArray(64, 'a') + "  " + kAppImageAsset.toUtf8() + '\n');
        const QString apps = appImageFolder();
        Harness h(testSetup(server, cache_->path(), appImage(apps)));
        h.controller->checkNow();
        QTRY_COMPARE(h.dialog()->state(), State::Available);
        h.button("updateDialog.install")->click();
        QTRY_COMPARE(h.dialog()->state(), State::Failed);
        QVERIFY2(h.dialog()->shownText().contains(QStringLiteral("Couldn't install GitBolt 0.1.1")),
                 qPrintable(h.dialog()->shownText()));
        QCOMPARE(entries(apps), QStringList{QStringLiteral("GitBolt.AppImage")});

        // Try Again installs again (it doesn't just check).
        const QByteArray assetPath = QStringLiteral("/download/v0.1.1/%1").arg(kAppImageAsset).toUtf8();
        const int before = server.hits(assetPath);
        h.button("updateDialog.retry")->click();
        QTRY_COMPARE(h.dialog()->state(), State::Failed);
        QCOMPARE(server.hits(assetPath), before + 1);
        QCOMPARE(server.hits(kFeedPath), 1);
    }

    // The new AppImage's name is the running one's: it would download
    // over itself. Refused before anything is asked for.
    void anAppImageNamedLikeTheNewOneIsRefused() {
        HttpServer server;
        serve(server, QStringLiteral("0.1.1"), {{kAppImageAsset, "new"}});
        const QString apps = appImageFolder(kAppImageAsset);
        Harness h(testSetup(server, cache_->path(), appImage(apps, kAppImageAsset)));
        h.controller->checkNow();
        QTRY_COMPARE(h.dialog()->state(), State::Available);
        h.button("updateDialog.install")->click();
        QCOMPARE(h.dialog()->state(), State::Failed);
        QVERIFY(h.dialog()->shownText().contains(QStringLiteral("has this one's name")));
        QCOMPARE(server.requests().size(), 1);
        QCOMPARE(contents(QDir(apps).filePath(kAppImageAsset)), QByteArray("old"));
    }

    void whatTheReleaseLacksIsSaid_data() {
        QTest::addColumn<bool>("withInstaller");
        QTest::addColumn<bool>("withChecksums");
        QTest::addColumn<QString>("says");
        QTest::newRow("no installer for this system") << false << true
                                                      << "no installer for this system (GitBolt-0.1.1-x86_64.AppImage)";
        QTest::newRow("no checksums") << true << false << "lists no checksums (SHA256SUMS.txt)";
    }
    void whatTheReleaseLacksIsSaid() {
        QFETCH(bool, withInstaller);
        QFETCH(bool, withChecksums);
        QFETCH(QString, says);
        HttpServer server;
        QList<QPair<QString, QByteArray>> assets{{QStringLiteral("GitBolt-0.1.1-Darwin-arm64.dmg"), "dmg"}};
        if (withInstaller)
            assets.append({kAppImageAsset, "new"});
        serve(server, QStringLiteral("0.1.1"), assets, withChecksums);
        Harness h(testSetup(server, cache_->path(), appImage(appImageFolder())));
        h.controller->checkNow();
        QTRY_COMPARE(h.dialog()->state(), State::NotInstallable);
        QVERIFY2(h.dialog()->shownText().contains(says), qPrintable(h.dialog()->shownText()));
    }

    // The deb's reason gives the apt command with the real file name.
    void theDebReasonNamesTheVersion() {
        HttpServer server;
        serve(server, QStringLiteral("0.1.1"), {{QStringLiteral("gitbolt_0.1.1_amd64.deb"), "deb"}});
        PackageInfo deb{PackageKind::Deb, QStringLiteral("/usr/lib/gitbolt"), false,
                        QStringLiteral("… from a terminal: sudo apt install ./gitbolt_<version>_amd64.deb")};
        Harness h(testSetup(server, cache_->path(), deb));
        h.controller->checkNow();
        QTRY_COMPARE(h.dialog()->state(), State::NotInstallable);
        QVERIFY2(h.dialog()->shownText().contains(QStringLiteral("sudo apt install ./gitbolt_0.1.1_amd64.deb")),
                 qPrintable(h.dialog()->shownText()));
    }

    // macOS: the download passes its checksum, but isn't a disk image
    // hdiutil attaches. The preparation fails, and the download goes.
    void aDiskImageThatWontAttachIsDeleted() {
#ifndef Q_OS_MACOS
        QSKIP("macOS: hdiutil");
#else
        HttpServer server;
        const QString dmg = QStringLiteral("GitBolt-0.1.1-Darwin-arm64.dmg");
        serve(server, QStringLiteral("0.1.1"), {{dmg, "not a disk image"}});
        QTemporaryDir apps;
        QVERIFY(QDir().mkpath(apps.filePath(QStringLiteral("GitBolt.app/Contents/MacOS"))));
        PackageInfo bundle{PackageKind::MacBundle, apps.filePath(QStringLiteral("GitBolt.app")), true, {}};
        Harness h(testSetup(server, cache_->path(), bundle));
        h.controller->checkNow();
        QTRY_COMPARE(h.dialog()->state(), State::Available);
        h.button("updateDialog.install")->click();
        QTRY_COMPARE_WITH_TIMEOUT(h.dialog()->state(), State::Failed, 30000);
        const QString version = QDir(h.controller->setup().config.downloadDir).filePath(QStringLiteral("0.1.1"));
        QCOMPARE(entries(version), QStringList{});
        QCOMPARE(entries(apps.path()), QStringList{QStringLiteral("GitBolt.app")});
#endif
    }

    // ---- what the dialog shows is what is acted on ----------------------------

    // The bar offers 0.1.1; a manual check is still out when the user
    // clicks the bar's View and then Install. The check's late answer (a
    // failure, or an older release) must not replace the install screen:
    // its Close would cancel the install.
    void viewDuringAManualCheckKeepsTheInstall_data() {
        QTest::addColumn<bool>("fails");
        QTest::newRow("the check fails") << true;
        QTest::newRow("the check finds nothing newer") << false;
    }
    void viewDuringAManualCheckKeepsTheInstall() {
        QFETCH(bool, fails);
        HttpServer server;
        serve(server, QStringLiteral("0.1.1"), {{kAppImageAsset, QByteArray(64 * 1024, 'n')}});
        // The download stops after a little: no progress() puts the
        // dialog right again.
        server.routes[QStringLiteral("/download/v0.1.1/%1").arg(kAppImageAsset).toUtf8()].stallAfter = 1024;
        const QString apps = appImageFolder();
        Harness h(testSetup(server, cache_->path(), appImage(apps)));

        h.fire();
        QTRY_COMPARE(h.bar->isHidden(), false);
        Served late = fails ? Served::withStatus(500, "{}")
                            : Served::json(releaseJson(server, QStringLiteral("0.0.9"), {}));
        late.delayMs = 700;
        server.routes.insert(kFeedPath, late);

        h.controller->checkNow();
        QCOMPARE(h.dialog()->state(), State::Checking);
        QTRY_COMPARE(server.hits(kFeedPath), 2);
        h.bar->findChild<QPushButton*>(QStringLiteral("updateNotice.view"))->click();
        QCOMPARE(h.dialog()->state(), State::Available);
        h.button("updateDialog.install")->click();
        QCOMPARE(h.dialog()->state(), State::Downloading);

        // The answer comes, and is let be.
        QTRY_VERIFY(!h.checker()->busy());
        QTest::qWait(100);
        QCOMPARE(h.dialog()->state(), State::Downloading);
        QVERIFY2(!h.dialog()->shownText().contains(QStringLiteral("Couldn't check")), qPrintable(h.dialog()->shownText()));
        QVERIFY(h.downloader()->busy());
        QCOMPARE(headline(h.dialog()), QStringLiteral("Downloading GitBolt 0.1.1…"));

        // Try Again isn't there to check again; Cancel goes back to the
        // release, and a later check still answers in the dialog.
        h.button("updateDialog.cancel")->click();
        QCOMPARE(h.dialog()->state(), State::Available);
        QCOMPARE(entries(apps), QStringList{QStringLiteral("GitBolt.AppImage")});
        server.routes.insert(kFeedPath, Served::withStatus(500, "{}"));
        h.controller->checkNow();
        QTRY_COMPARE(h.dialog()->state(), State::Failed);
        QVERIFY(h.dialog()->shownText().contains(QStringLiteral("Couldn't check")));
    }

    // The dialog shows 0.1.1 when the daily check finds 0.1.2: Install
    // and Skip still mean 0.1.1, and the bar offers 0.1.2 once the
    // dialog closes.
    void theDialogsReleaseIsTheOneActedOn() {
        HttpServer server;
        serve(server, QStringLiteral("0.1.1"), {{kAppImageAsset, QByteArray(256 * 1024, 'n')}});
        const QByteArray oldAsset = QStringLiteral("/download/v0.1.1/%1").arg(kAppImageAsset).toUtf8();
        server.routes[oldAsset].chunkBytes = 4096;
        server.routes[oldAsset].chunkDelayMs = 40;
        const QString apps = appImageFolder();
        Harness h(testSetup(server, cache_->path(), appImage(apps)));
        h.controller->checkNow();
        QTRY_COMPARE(h.dialog()->state(), State::Available);
        QCOMPARE(headline(h.dialog()), QStringLiteral("GitBolt 0.1.1 is available"));

        const QString newAsset = QStringLiteral("GitBolt-0.1.2-x86_64.AppImage");
        serve(server, QStringLiteral("0.1.2"), {{newAsset, "newer"}});
        h.settings.setUpdateLastCheck({});
        h.fire();
        QTRY_COMPARE(server.hits(kFeedPath), 2);
        QTRY_VERIFY(!h.checker()->busy());
        QCOMPARE(headline(h.dialog()), QStringLiteral("GitBolt 0.1.1 is available"));
        QVERIFY(h.bar->isHidden());   // the dialog is up

        h.button("updateDialog.install")->click();
        QCOMPARE(headline(h.dialog()), QStringLiteral("Downloading GitBolt 0.1.1…"));
        QTRY_COMPARE(server.hits(oldAsset), 1);
        QCOMPARE(server.hits(QStringLiteral("/download/v0.1.2/%1").arg(newAsset).toUtf8()), 0);
        h.button("updateDialog.cancel")->click();
        QCOMPARE(h.dialog()->state(), State::Available);
        QCOMPARE(headline(h.dialog()), QStringLiteral("GitBolt 0.1.1 is available"));

        h.button("updateDialog.skip")->click();
        QCOMPARE(h.settings.skippedUpdateVersion(), QStringLiteral("0.1.1"));
        QVERIFY(!h.dialog()->isVisible());
        QVERIFY(!h.bar->isHidden());
        QCOMPARE(h.bar->version(), QStringLiteral("0.1.2"));
    }

    // ---- once the helper runs ------------------------------------------------

    // A real helper for a stand-in AppImage, waiting on a child process
    // in this process's place; the quit is caught, and never happens.
    void afterTheHelperStartsNothingIsLeftToCancel() {
#if defined(Q_OS_WIN)
        QSKIP("the AppImage helper is a /bin/sh script");
#else
        HttpServer server;
        // The "new AppImage": when the helper starts it, it leaves a
        // marker, so the test knows the helper is done.
        const QByteArray script = "#!/bin/sh\necho started > \"$(dirname \"$0\")/started\"\n";
        serve(server, QStringLiteral("0.1.1"), {{kAppImageAsset, script}});
        const QString apps = appImageFolder();
        QProcess stand;   // GitBolt, for the helper to wait for
        stand.start(QStringLiteral("/bin/sleep"), {QStringLiteral("120")});
        QVERIFY(stand.waitForStarted());
        auto standDown = qScopeGuard([&] {
            stand.kill();
            stand.waitForFinished();
        });
        {
            Harness h(testSetup(server, cache_->path(), appImage(apps)));
            int quits = 0;
            h.controller->setRestartHooks({[&] { return stand.processId(); }, [&] { ++quits; }, 300});
            h.controller->checkNow();
            QTRY_COMPARE(h.dialog()->state(), State::Available);
            h.button("updateDialog.install")->click();

            QTRY_COMPARE_WITH_TIMEOUT(h.dialog()->state(), State::Restarting, 15000);
            QCOMPARE(quits, 1);
            QCOMPARE(shownButtons(h.dialog()), QStringList{});
            QCOMPARE(headline(h.dialog()), QStringLiteral("Restarting GitBolt to finish the update…"));
            const QDir downloads(h.controller->setup().config.downloadDir);
            QVERIFY(QFile::exists(downloads.filePath(QStringLiteral("install-helper.sh"))));
            QVERIFY(QFile::exists(downloads.filePath(QStringLiteral("install.log"))));

            // Still here after the grace time: the quit was refused.
            QTRY_COMPARE(h.dialog()->state(), State::QuitNeeded);
            QCOMPARE(headline(h.dialog()), QStringLiteral("Quit GitBolt to finish installing 0.1.1."));
            QCOMPARE(shownButtons(h.dialog()),
                     (QStringList{QStringLiteral("updateDialog.close"), QStringLiteral("updateDialog.quit")}));
            QVERIFY(h.dialog()->shownText().contains(QStringLiteral("within 10 minutes")));
            h.button("updateDialog.quit")->click();
            QCOMPARE(quits, 2);
            QCOMPARE(h.dialog()->state(), State::Restarting);
            QTRY_COMPARE(h.dialog()->state(), State::QuitNeeded);

            // Nothing starts over: no check, no second install.
            h.controller->checkNow();
            QCOMPARE(h.dialog()->state(), State::QuitNeeded);
            QCOMPARE(server.hits(kFeedPath), 1);

            // Closed, or gone with the window: the download is the
            // helper's, and stays.
            h.dialog()->reject();
        }
        QCOMPARE(entries(apps), (QStringList{kAppImageAsset, QStringLiteral("GitBolt.AppImage")}));
        QCOMPARE(contents(QDir(apps).filePath(QStringLiteral("GitBolt.AppImage"))), QByteArray("old"));

        // "GitBolt" quits: the helper puts the new AppImage in place and
        // starts it.
        stand.kill();
        stand.waitForFinished();
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(QDir(apps).filePath(QStringLiteral("started"))), 15000);
        QCOMPARE(contents(QDir(apps).filePath(QStringLiteral("GitBolt.AppImage"))), script);
#endif
    }

    // ---- after an install: the helper's install-result -----------------------

    void anEarlierFailureIsShown_data() {
        QTest::addColumn<QByteArray>("result");
        QTest::addColumn<int>("kind");
        QTest::addColumn<QStringList>("says");
        QTest::addColumn<QString>("offerSays");

        QTest::newRow("Windows: the installer failed")
            << QByteArray("failed 0.1.1 the installer exited with code 2\n") << int(PackageKind::WindowsInstall)
            << QStringList{QStringLiteral("Installing GitBolt 0.1.1 failed after GitBolt quit: the installer "
                                          "exited with code 2."),
                           QStringLiteral("download GitBolt-0.1.1-Windows-AMD64.exe from the release page and run "
                                          "it as an administrator.")}
            << "Windows will ask for permission to install it.";
        // PowerShell's way: a BOM, CRLF.
        QTest::newRow("Windows: UAC declined")
            << QByteArray("\xEF\xBB\xBF" "declined 0.1.1 the request for permission was cancelled\r\n")
            << int(PackageKind::WindowsInstall)
            << QStringList{QStringLiteral("needs an administrator's permission, and it wasn't given: the request "
                                          "for permission was cancelled.")}
            << "Windows will ask for permission to install it.";
        QTest::newRow("deb: polkit dismissed") << QByteArray("declined 0.1.1\n") << int(PackageKind::Deb)
                                               << QStringList{QStringLiteral("and it wasn't given."),
                                                              QStringLiteral("sudo apt install ./gitbolt_0.1.1_amd64.deb")}
                                               << "You'll be asked for an administrator's password.";
        QTest::newRow("deb: no polkit agent")
            << QByteArray("failed 0.1.1 no polkit authentication agent is running\n") << int(PackageKind::Deb)
            << QStringList{QStringLiteral("failed after GitBolt quit: no polkit authentication agent is running."),
                           QStringLiteral("sudo apt install ./gitbolt_0.1.1_amd64.deb")}
            << "You'll be asked for an administrator's password.";
        QTest::newRow("macOS: rolled back")
            << QByteArray("failed 0.1.1 couldn't move the new copy into place; the old one was put back.\n")
            << int(PackageKind::MacBundle)
            << QStringList{QStringLiteral("the old one was put back."),
                           QStringLiteral("download GitBolt-0.1.1-Darwin-arm64.dmg from the release page, open it, "
                                          "and drag GitBolt into your Applications folder.")}
            << "restarts GitBolt with it.";
        QTest::newRow("AppImage: gave up") << QByteArray("gave-up 0.1.1 ") << int(PackageKind::AppImage)
                                           << QStringList{QStringLiteral("GitBolt didn't quit within 10 minutes"),
                                                          QStringLiteral("download GitBolt-0.1.1-x86_64.AppImage")}
                                           << "restarts GitBolt with it.";
    }
    void anEarlierFailureIsShown() {
        QFETCH(QByteArray, result);
        QFETCH(int, kind);
        QFETCH(QStringList, says);
        QFETCH(QString, offerSays);
        HttpServer server;
        serve(server, QStringLiteral("0.1.1"), kEveryInstaller);
        UrlCatcher catcher;
        const PackageKind packageKind = PackageKind(kind);
        UpdateSetup setup = testSetup(server, cache_->path(),
                                      {packageKind, QDir(cache_->path()).filePath(QStringLiteral("target")), true, {}});
        const QDir downloads(setup.config.downloadDir);
        QVERIFY(writeFile(downloads.filePath(QStringLiteral("install-result")), result));
        QByteArray log;
        for (int i = 1; i <= 15; ++i)
            log += "2026-11-07 10:00:" + QByteArray::number(i).rightJustified(2, '0') + " step " + QByteArray::number(i) + '\n';
        QVERIFY(writeFile(downloads.filePath(QStringLiteral("install.log")), log));

        Harness h(setup);
        QSignalSpy status(h.controller, &UpdateController::statusMessage);
        h.controller->start();
        QVERIFY(h.dialog() && h.dialog()->isVisible());
        QCOMPARE(h.dialog()->state(), State::Failed);
        QCOMPARE(headline(h.dialog()), QStringLiteral("Couldn't install GitBolt 0.1.1"));
        const QString shown = h.dialog()->shownText();
        for (const QString& line : says)
            QVERIFY2(shown.contains(line), qPrintable(shown));
        QVERIFY2(shown.contains(QStringLiteral("GitBolt wasn't changed.")), qPrintable(shown));
        QVERIFY2(shown.contains(QDir::toNativeSeparators(downloads.filePath(QStringLiteral("install.log")))),
                 qPrintable(shown));
        // The log's last ten lines, as written.
        auto* notes = h.dialog()->findChild<QPlainTextEdit*>(QStringLiteral("updateDialog.notes"));
        QVERIFY(!notes->isHidden());
        const QStringList lines = notes->toPlainText().split(QLatin1Char('\n'));
        QCOMPARE(lines.size(), 10);
        QVERIFY(lines.first().endsWith(QStringLiteral("step 6")));
        QVERIFY(lines.last().endsWith(QStringLiteral("step 15")));
        QCOMPARE(status.count(), 0);
        // Read once: gone, and nothing asked of the network.
        QVERIFY(!QFile::exists(downloads.filePath(QStringLiteral("install-result"))));
        QVERIFY(QFile::exists(downloads.filePath(QStringLiteral("install.log"))));
        QCOMPARE(server.requests().size(), 0);

        // That version's release page.
        h.button("updateDialog.openReleasePage")->click();
        QCOMPARE(catcher.urls, QList<QUrl>{QUrl(QStringLiteral("https://github.com/Inverian/GitBolt/releases/tag/v0.1.1"))});
        // Try Again asks again, and offers it once more, saying what
        // comes (the system's prompt) this time.
        h.button("updateDialog.retry")->click();
        QTRY_COMPARE(h.dialog()->state(), State::Available);
        QCOMPARE(server.hits(kFeedPath), 1);
        QVERIFY2(h.dialog()->shownText().contains(offerSays), qPrintable(h.dialog()->shownText()));
        QVERIFY(notes->toPlainText().contains(QStringLiteral("Fixed.")));

        // The next start, in the same folder, says nothing.
        Harness again(setup);
        again.controller->start();
        QVERIFY(!again.dialog());
    }

    // No log (it was deleted, or never written): the dialog says the rest.
    void anEarlierFailureWithoutALog() {
        HttpServer server;
        UpdateSetup setup = testSetup(server, cache_->path());
        QVERIFY(writeFile(QDir(setup.config.downloadDir).filePath(QStringLiteral("install-result")),
                          "failed 0.1.1 apt-get failed with status 100\n"));
        Harness h(setup);
        h.controller->start();
        QVERIFY(h.dialog());
        QCOMPARE(h.dialog()->state(), State::Failed);
        QVERIFY(h.dialog()->findChild<QPlainTextEdit*>(QStringLiteral("updateDialog.notes"))->isHidden());
        QVERIFY(!h.dialog()->shownText().contains(QStringLiteral("install.log")));
        QVERIFY(h.dialog()->shownText().contains(QStringLiteral("apt-get failed with status 100.")));
    }

    void anEarlierUpdateIsReported_data() {
        QTest::addColumn<QByteArray>("result");
        QTest::addColumn<QString>("says");
        QTest::newRow("this version") << QByteArray("ok 0.1.0 \n") << "GitBolt was updated to 0.1.0.";
        // A test session's relaunched copy goes on being 0.1.0
        // (GITBOLT_TEST_APP_VERSION), whatever it installed.
        QTest::newRow("a newer one") << QByteArray("ok 0.1.9\n") << "GitBolt was updated to 0.1.9.";
        QTest::newRow("stale: an older one") << QByteArray("ok 0.0.9\n") << "";
    }
    void anEarlierUpdateIsReported() {
        QFETCH(QByteArray, result);
        QFETCH(QString, says);
        HttpServer server;
        UpdateSetup setup = testSetup(server, cache_->path());
        const QString path = QDir(setup.config.downloadDir).filePath(QStringLiteral("install-result"));
        QVERIFY(writeFile(path, result));
        Harness h(setup);
        QSignalSpy status(h.controller, &UpdateController::statusMessage);
        h.controller->start();
        QCOMPARE(status.count(), says.isEmpty() ? 0 : 1);
        if (!says.isEmpty())
            QCOMPARE(status.first().first().toString(), says);
        QVERIFY(!h.dialog());
        QVERIFY(!QFile::exists(path));
    }

    void aStaleOrUnreadableResultIsDropped_data() {
        QTest::addColumn<QByteArray>("result");
        // Stale: this version, or an older one, runs now.
        QTest::newRow("failed, this version") << QByteArray("failed 0.1.0 the installer exited with code 2\n");
        QTest::newRow("declined, an older one") << QByteArray("declined 0.0.9\n");
        QTest::newRow("gave up, this version") << QByteArray("gave-up 0.1.0\n");
        // Not an install result.
        QTest::newRow("empty") << QByteArray();
        QTest::newRow("blank") << QByteArray(" \n\n");
        QTest::newRow("a word") << QByteArray("garbage\n");
        QTest::newRow("no version") << QByteArray("failed\n");
        QTest::newRow("a short version") << QByteArray("failed 0.1 reason\n");
        QTest::newRow("a tag, not a version") << QByteArray("failed v0.1.1 reason\n");
        QTest::newRow("an unknown status") << QByteArray("maybe 0.1.1 reason\n");
        QTest::newRow("a status in capitals") << QByteArray("FAILED 0.1.1\n");
        QTest::newRow("two lines") << QByteArray("failed 0.1.1 one\nfailed 0.1.1 two\n");
        QTest::newRow("too long") << QByteArray("failed 0.1.1 ") + QByteArray(5000, 'x') + '\n';
        QTest::newRow("binary") << QByteArray("\x00\x01\x02\xff failed 0.1.1", 17);
    }
    void aStaleOrUnreadableResultIsDropped() {
        QFETCH(QByteArray, result);
        HttpServer server;
        UpdateSetup setup = testSetup(server, cache_->path(), appImage(appImageFolder()));
        const QString path = QDir(setup.config.downloadDir).filePath(QStringLiteral("install-result"));
        QVERIFY(writeFile(path, result));
        Harness h(setup);
        QSignalSpy status(h.controller, &UpdateController::statusMessage);
        h.controller->start();
        QVERIFY(!h.dialog());
        QCOMPARE(status.count(), 0);
        QVERIFY(!QFile::exists(path));
    }

    // ---- MainWindow -----------------------------------------------------------

    // After an update, the window's status bar says so.
    void anUpdateIsReportedInTheStatusBar() {
        HttpServer server;
        gitbolt::ui::MainWindow window;
        UpdateSetup setup = testSetup(server, cache_->path());
        QVERIFY(writeFile(QDir(setup.config.downloadDir).filePath(QStringLiteral("install-result")), "ok 0.1.0\n"));
        window.setUpdates(setup);
        window.startUpdates();
        QCOMPARE(window.statusBar()->currentMessage(), QStringLiteral("GitBolt was updated to 0.1.0."));
        QCOMPARE(window.updateDownloadDir(), setup.config.downloadDir);
        QVERIFY(!window.findChild<UpdateDialog*>());
    }

    void withoutTheCheckTheMenuOpensTheReleasesPage_data() {
        QTest::addColumn<bool>("testSession");
        QTest::newRow("source build") << false;
        QTest::newRow("test session without a test feed") << true;
    }
    void withoutTheCheckTheMenuOpensTheReleasesPage() {
        QFETCH(bool, testSession);
        UrlCatcher catcher;
        gitbolt::ui::MainWindow window;
        if (testSession) {
            gitbolt::ui::UpdateSetupInputs inputs;
            inputs.testSession = gitbolt::util::TestSessionKind::Bridge;
            inputs.appVersion = QStringLiteral("0.1.0");
            inputs.cacheDir = cache_->path();
            inputs.tempDir = cache_->path();
            window.setUpdates(gitbolt::ui::setUpUpdates(inputs));
            QCOMPARE(window.updateGate(), UpdateGate::TestSession);
        } else {
            QCOMPARE(window.updateGate(), UpdateGate::CompiledOut);
        }
        window.startUpdates();
        auto* action = window.findChild<QAction*>(QStringLiteral("help.check-for-updates"));
        QVERIFY(action);
        action->trigger();
        QCOMPARE(catcher.urls, QList<QUrl>{QUrl(QStringLiteral("https://github.com/Inverian/GitBolt/releases"))});
        QVERIFY(!window.findChild<UpdateDialog*>());
        QVERIFY(!window.findChild<QTimer*>(QStringLiteral("updateCheckTimer")));
        QVERIFY(!QDir(cache_->path()).exists(QStringLiteral("updates")));
        // No downloads folder of a test session either.
        QCOMPARE(QDir(cache_->path()).entryList({QStringLiteral("gitbolt-updates*")},
                                                 QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot),
                 QStringList{});
    }

    void withTheCheckTheMenuAsks() {
        HttpServer server;
        serve(server, QStringLiteral("0.1.1"), {{kAppImageAsset, "new"}});
        UrlCatcher catcher;
        gitbolt::ui::MainWindow window;
        window.setUpdates(testSetup(server, cache_->path()));
        QCOMPARE(window.updateGate(), UpdateGate::TestFeed);
        // The bar sits above the views, hidden until there's news.
        auto* bar = window.findChild<UpdateNoticeBar*>(QStringLiteral("updateNotice"));
        QVERIFY(bar && bar->isHidden());

        window.findChild<QAction*>(QStringLiteral("help.check-for-updates"))->trigger();
        auto* dialog = window.findChild<UpdateDialog*>();
        QVERIFY(dialog && dialog->isVisible());
        QTRY_COMPARE(dialog->state(), State::NotInstallable);
        QCOMPARE(server.hits(kFeedPath), 1);
        QVERIFY(catcher.urls.isEmpty());
        dialog->reject();

        // The daily check's timer, for the bridge.
        auto* timer = window.findChild<QTimer*>(QStringLiteral("updateCheckTimer"));
        QVERIFY(timer && !timer->isActive());
    }

private:
    // A folder with a running AppImage stand-in in it, named `name`.
    QString appImageFolder(const QString& name = QStringLiteral("GitBolt.AppImage")) {
        const QString apps = QDir(cache_->path()).filePath(QStringLiteral("apps"));
        QDir().mkpath(apps);
        QFile file(QDir(apps).filePath(name));
        if (file.open(QIODevice::WriteOnly))
            file.write("old");
        return apps;
    }

    static PackageInfo appImage(const QString& folder, const QString& name = QStringLiteral("GitBolt.AppImage")) {
        return {PackageKind::AppImage, QDir(folder).filePath(name), true, {}};
    }

    QTemporaryDir settingsDir_;
    std::unique_ptr<QTemporaryDir> cache_;
};

QTEST_MAIN(TestUpdateController)
#include "TestUpdateController.moc"
