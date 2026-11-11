//
// TestTelemetrySwitch — the Settings switch reaches usage statistics
// through the real MainWindow wiring, against a loopback HTTP stub
// (../telemetry/HttpStub.h), never a real endpoint.
//
// "Off stops sending at once and deletes the queue" only holds because
// MainWindow listens to its OWN SettingsService, the one the Settings
// dialog writes through: settingsChanged is emitted per instance, so a
// service wired to main()'s instance would never hear the switch. The
// telemetry unit tests call setUserEnabled() directly and e2e sessions
// are inert, so only this test would catch that regression:
//
//   - off, applied the way the dialog applies it (a batch on the
//     window's SettingsService): the queued record's file is gone, and
//     nothing reaches the stub, not even a counted action;
//   - on again: menu actions are counted by objectName and sent;
//   - in-app updates reach the same service: an earlier install's
//     outcome (update_result, and update_failed for a failure), the
//     update dialog's buttons, and on macOS the application menu's items
//     (a menu bar of their own, outside the window's).
//

#include <QAction>
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>

#include "../telemetry/HttpStub.h"
#include "../update/HttpServer.h"
#include "AppMenuWatcher.h"
#include "conf/SettingsService.h"
#include "dialogs/UpdateDialog.h"
#include "telemetry/EventQueue.h"
#include "telemetry/TelemetryConfig.h"
#include "telemetry/TelemetryGate.h"
#include "telemetry/TelemetryService.h"
#include "ui/MainWindow.h"
#include "ui/UpdateSetup.h"

#include <utility>

using namespace gitbolt::telemetry;
using gitbolt::conf::SettingsService;
using gitbolt::test::HttpStub;
using gitbolt::test::StubRequest;

namespace {

const QString kInstallId = QStringLiteral("1b4e28ba-2fa1-4d2c-883f-0016d3cca427");

// How many triggers of `command` the stub's command_used events add
// up to.
int commandCount(const HttpStub& stub, const QString& command) {
    int n = 0;
    for (const StubRequest& request : stub.requests()) {
        const QJsonObject payload = request.json().value(QStringLiteral("payload")).toObject();
        const QJsonObject properties = payload.value(QStringLiteral("properties")).toObject();
        if (payload.value(QStringLiteral("name")).toString() == QLatin1String("command_used")
            && properties.value(QStringLiteral("command")).toString() == command)
            n += properties.value(QStringLiteral("count")).toInt();
    }
    return n;
}

// What the stub got for `event`: an OpenPanel event's properties by its
// name, or a GlitchTip error's tags by their kind.
QList<QJsonObject> sent(const HttpStub& stub, const QString& event) {
    QList<QJsonObject> found;
    for (const StubRequest& request : stub.requests()) {
        const QJsonObject body = request.json();
        const QJsonObject payload = body.value(QStringLiteral("payload")).toObject();
        if (payload.value(QStringLiteral("name")).toString() == event)
            found << payload.value(QStringLiteral("properties")).toObject();
        else if (body.value(QStringLiteral("tags")).toObject().value(QStringLiteral("kind")).toString() == event)
            found << body.value(QStringLiteral("tags")).toObject();
    }
    return found;
}

// Short timings; the queue in `dir`.
Config testConfig(const HttpStub& stub, const QString& dir) {
    Config config = Config::fromEndpoints(stub.origin(), QStringLiteral("test-client"),
                                          QStringLiteral("test-key-value"),
                                          QStringLiteral("0123456789abcdef0123456789abcdef"), QStringLiteral("42"),
                                          QStringLiteral("0.1.0"), dir);
    config.paceMs = 50;
    config.transferTimeoutMs = 300;
    config.firstFlushDelayMs = 0;
    config.countWindowMs = 200;
    return config;
}

bool writeFile(const QString& path, const QByteArray& bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size();
}

// A test session's updates setup, its feed `feed`, running 0.1.0.
gitbolt::ui::UpdateSetup updateSetup(const QUrl& feed, const QString& dir,
                                     gitbolt::update::PackageInfo package = {}) {
    gitbolt::ui::UpdateSetupInputs inputs;
    inputs.testSession = gitbolt::util::TestSessionKind::Bridge;
    inputs.env.insert(QStringLiteral("GITBOLT_TEST_UPDATE_FEED"), feed.toString());
    inputs.env.insert(QStringLiteral("GITBOLT_TEST_APP_VERSION"), QStringLiteral("0.1.0"));
    inputs.appVersion = QStringLiteral("0.1.5");
    inputs.cacheDir = dir;
    inputs.tempDir = dir;
    gitbolt::ui::UpdateSetup setup = gitbolt::ui::setUpUpdates(inputs);
    setup.package = std::move(package);
    return setup;
}

} // namespace

class TestTelemetrySwitch : public QObject {
    Q_OBJECT

private slots:
    // GitBolt.ini and the telemetry queue in folders of the test's own.
    void initTestCase() {
        QVERIFY(settingsDir_.isValid());
        QVERIFY(dataDir_.isValid());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir_.path());
    }

    void switchOffDiscardsAndStopsThenOnResumes() {
        HttpStub stub;
        QVERIFY(stub.isListening());
        Config config = Config::fromEndpoints(
            stub.origin(), QStringLiteral("test-client"), QStringLiteral("test-key-value"),
            QStringLiteral("0123456789abcdef0123456789abcdef"), QStringLiteral("42"),
            QStringLiteral("0.1.0"), dataDir_.filePath(QStringLiteral("telemetry")));
        config.paceMs = 50;
        config.transferTimeoutMs = 300;
        config.firstFlushDelayMs = 0;
        config.countWindowMs = 200;
        QVERIFY(config.isComplete());

        // As in main(): the service is declared first and outlives the
        // window, which keeps a raw pointer to it.
        TelemetryService service(config, [] { return kInstallId; }, true);
        gitbolt::ui::MainWindow window;
        window.setTelemetry(&service, Gate::Active);
        QCOMPARE(window.telemetryGate(), Gate::Active);
        auto* home = window.findChild<QAction*>(QStringLiteral("file.home"));
        QVERIFY(home);

        // One record queued and on disk, not sent: start() hasn't run.
        service.recordLaunch();
        QCOMPARE(service.queuedCount(), 1);
        const QString queueFile = EventQueue::filePath(config.queueDir);
        QVERIFY(QFile::exists(queueFile));

        // Off, as the Settings dialog's Apply does it: one batch on the
        // window's own SettingsService (the one findChild finds, as the
        // test bridge does).
        auto* settings = window.findChild<SettingsService*>();
        QVERIFY(settings);
        settings->beginUpdate();
        settings->setUsageStatisticsEnabled(false);
        QVERIFY(QFile::exists(queueFile));   // nothing until the batch closes
        settings->endUpdate();
        QVERIFY(!service.isActive());
        QVERIFY(!QFile::exists(queueFile));

        // Nothing reaches the stub: not the discarded record, not an
        // action triggered while off.
        service.start();
        home->trigger();
        QTest::qWait(500);
        QCOMPARE(stub.requests().size(), 0);

        // On again: sending resumes, and the window counts its actions
        // by objectName.
        settings->beginUpdate();
        settings->setUsageStatisticsEnabled(true);
        settings->endUpdate();
        QVERIFY(service.isActive());
        home->trigger();
        home->trigger();
        QTRY_COMPARE(commandCount(stub, QStringLiteral("file.home")), 2);
        // Commands › Revert commit too, a menu-bar action. Off with no
        // repository open (a trigger then does nothing): turned on here,
        // it opens nothing and is counted all the same.
        auto* revert = window.findChild<QAction*>(QStringLiteral("commands.revert-commit"));
        QVERIFY(revert);
        revert->setEnabled(true);
        revert->trigger();
        QTRY_COMPARE(commandCount(stub, QStringLiteral("commands.revert-commit")), 1);
        for (const StubRequest& request : stub.requests())
            QCOMPARE(request.path, QByteArray("/track"));
        QVERIFY2(stub.headerViolations().isEmpty(),
                 qPrintable(stub.headerViolations().join(QLatin1Char('\n'))));
    }

    // An install that failed after the quit, as the relaunched copy reads
    // it at startUpdates() (install-result, and the marker the launch
    // left): update_result and an update_failed error, fixed codes only.
    // Closing the dialog it opens is counted as its button's name.
    void updateOutcomesReachTheService() {
        HttpStub stub;
        QVERIFY(stub.isListening());
        TelemetryService service(testConfig(stub, dataDir_.filePath(QStringLiteral("outcomes"))),
                                 [] { return kInstallId; }, true);
        gitbolt::ui::MainWindow window;
        window.setTelemetry(&service, Gate::Active);
        QTemporaryDir updates;
        QVERIFY(updates.isValid());
        const gitbolt::ui::UpdateSetup setup =
            updateSetup(QUrl(QStringLiteral("http://127.0.0.1:9/repos/Inverian/GitBolt/releases/latest")),
                        updates.path(),
                        {gitbolt::update::PackageKind::Deb, updates.filePath(QStringLiteral("lib")), true, {}});
        const QDir downloads(setup.config.downloadDir);
        QVERIFY(writeFile(downloads.filePath(QStringLiteral("install-result")),
                          "failed 0.1.1 apt-get failed with status 100\n"));
        QVERIFY(writeFile(downloads.filePath(QStringLiteral("pending-install")),
                          "0.1.0 0.1.1 " + QByteArray::number(QDateTime::currentSecsSinceEpoch()) + '\n'));
        window.setUpdates(setup);
        service.start();
        window.startUpdates();
        auto* dialog = window.findChild<gitbolt::dialogs::UpdateDialog*>();
        QVERIFY(dialog && dialog->isVisible());
        dialog->findChild<QPushButton*>(QStringLiteral("updateDialog.close"))->click();

        QTRY_COMPARE(sent(stub, QStringLiteral("update_result")).size(), 1);
        const QJsonObject result = sent(stub, QStringLiteral("update_result")).first();
        for (const auto& [key, value] : {std::pair{"status", "failed"}, {"from_version", "0.1.0"},
                                         {"to_version", "0.1.1"}, {"package", "deb"}, {"stage", "install"},
                                         {"reason", "apt_failed"}})
            QCOMPARE(result.value(QLatin1String(key)).toString(), QLatin1String(value));
        QTRY_COMPARE(sent(stub, QStringLiteral("update_failed")).size(), 1);
        QCOMPARE(sent(stub, QStringLiteral("update_failed")).first(),
                 (QJsonObject{{QStringLiteral("kind"), QStringLiteral("update_failed")},
                              {QStringLiteral("stage"), QStringLiteral("install")},
                              {QStringLiteral("code"), QStringLiteral("apt_failed")}}));
        QTRY_COMPARE(commandCount(stub, QStringLiteral("updateDialog.close")), 1);
        QVERIFY(!QFileInfo::exists(downloads.filePath(QStringLiteral("pending-install"))));
        QVERIFY2(stub.headerViolations().isEmpty(),
                 qPrintable(stub.headerViolations().join(QLatin1Char('\n'))));
    }

    // macOS: the application menu's Check for Updates… is counted, by
    // its own name, like the window's menus.
    void appMenuItemsAreCounted() {
#ifndef Q_OS_MACOS
        QSKIP("macOS: the application menu");
#else
        HttpStub stub;
        QVERIFY(stub.isListening());
        gitbolt::test::HttpServer feed;
        const QByteArray feedPath = "/repos/Inverian/GitBolt/releases/latest";
        feed.routes.insert(feedPath,
                           gitbolt::test::Served::json(QJsonDocument(QJsonObject{
                               {QStringLiteral("tag_name"), QStringLiteral("v0.1.1")},
                               {QStringLiteral("name"), QStringLiteral("GitBolt 0.1.1")},
                               {QStringLiteral("draft"), false},
                               {QStringLiteral("prerelease"), false},
                               {QStringLiteral("html_url"),
                                QStringLiteral("https://github.com/Inverian/GitBolt/releases/tag/v0.1.1")},
                               {QStringLiteral("body"), QStringLiteral("Notes.")},
                           }).toJson()));
        TelemetryService service(testConfig(stub, dataDir_.filePath(QStringLiteral("app-menu"))),
                                 [] { return kInstallId; }, true);
        gitbolt::test::AppMenuWatcher watcher;
        gitbolt::ui::MainWindow window;
        window.setTelemetry(&service, Gate::Active);
        QTemporaryDir updates;
        QVERIFY(updates.isValid());
        window.setUpdates(updateSetup(feed.url(QString::fromLatin1(feedPath)), updates.path()));
        QAction* check = watcher.named(QStringLiteral("help.check-for-updates-app-menu"));
        QVERIFY(check && check->isEnabled());
        service.start();
        check->trigger();
        auto* dialog = window.findChild<gitbolt::dialogs::UpdateDialog*>();
        QVERIFY(dialog);
        QTRY_COMPARE(dialog->state(), gitbolt::dialogs::UpdateDialog::State::NotInstallable);
        QTRY_COMPARE(commandCount(stub, QStringLiteral("help.check-for-updates-app-menu")), 1);
        QCOMPARE(feed.hits(feedPath), 1);
#endif
    }

private:
    QTemporaryDir settingsDir_;
    QTemporaryDir dataDir_;
};

QTEST_MAIN(TestTelemetrySwitch)
#include "TestTelemetrySwitch.moc"
