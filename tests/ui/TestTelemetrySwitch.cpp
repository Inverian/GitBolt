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
//     (a menu bar of their own, outside the window's);
//   - right-click menus' entries, each once by its own name, Qt's own
//     (Select All) and a toolbar's » menu never.
//

#include <QAction>
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QTableView>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>

#include "../TestRepoHelper.h"
#include "../telemetry/HttpStub.h"
#include "../update/HttpServer.h"
#include "AppMenuWatcher.h"
#include "conf/SettingsService.h"
#include "dialogs/CherryPickDialog.h"
#include "dialogs/UpdateDialog.h"
#include "git/GitProcess.h"
#include "services/GitService.h"
#include "telemetry/EventQueue.h"
#include "telemetry/TelemetryConfig.h"
#include "telemetry/TelemetryGate.h"
#include "telemetry/TelemetryService.h"
#include "ui/MainWindow.h"
#include "ui/UpdateSetup.h"
#include "widgets/WorktreeWidget.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

using namespace gitbolt::telemetry;
using gitbolt::conf::SettingsService;
using gitbolt::services::GitService;
using gitbolt::test::HttpStub;
using gitbolt::test::StubRequest;

namespace {

const QString kInstallId = QStringLiteral("1b4e28ba-2fa1-4d2c-883f-0016d3cca427");

// Every command the stub's command_used events name, with the triggers
// they add up to.
QHash<QString, int> commandCounts(const HttpStub& stub) {
    QHash<QString, int> counts;
    for (const StubRequest& request : stub.requests()) {
        const QJsonObject payload = request.json().value(QStringLiteral("payload")).toObject();
        const QJsonObject properties = payload.value(QStringLiteral("properties")).toObject();
        if (payload.value(QStringLiteral("name")).toString() == QLatin1String("command_used"))
            counts[properties.value(QStringLiteral("command")).toString()] +=
                properties.value(QStringLiteral("count")).toInt();
    }
    return counts;
}

int commandCount(const HttpStub& stub, const QString& command) {
    return commandCounts(stub).value(command);
}

// Opens a right-click menu with `open` (back once the menu is closed)
// and clicks its entry `name`, as the user would, once it is up.
bool clickInMenu(const std::function<void()>& open, const QString& name) {
    bool clicked = false;
    QTimer timer;
    timer.setInterval(20);
    QObject::connect(&timer, &QTimer::timeout, [&] {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        if (!menu)
            return;
        timer.stop();
        for (QAction* entry : menu->actions()) {
            if (entry->objectName() == name && entry->isEnabled())
                QTest::mouseClick(menu, Qt::LeftButton, {}, menu->actionGeometry(entry).center());
        }
        clicked = !menu->isVisible();   // a click on an entry closes it
        menu->close();
    });
    timer.start();
    open();
    return clicked;
}

// Right-clicks `view`'s row `index` and clicks the menu's entry `name`.
bool clickInRowMenu(QAbstractItemView* view, const QModelIndex& index, const QString& name) {
    view->scrollTo(index);
    const QPoint pos = view->visualRect(index).center();
    return view->indexAt(pos) == index
        && clickInMenu([&] { emit view->customContextMenuRequested(pos); }, name);
}

// The row of `name` in the branch tree, under whatever category.
QModelIndex rowOf(const QAbstractItemModel* model, const QString& name, const QModelIndex& parent = {}) {
    for (int row = 0; row < model->rowCount(parent); ++row) {
        const QModelIndex index = model->index(row, 0, parent);
        if (parent.isValid() && index.data().toString() == name)
            return index;
        if (const QModelIndex below = rowOf(model, name, index); below.isValid())
            return below;
    }
    return {};
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

    // Right-click menus, made anew for each click: an entry counts once,
    // by its own name (the log's Revert, the branch tree's Checkout);
    // Qt's own entries (the diff's Select All) not at all; a menu-bar
    // action in such a menu once, not once per menu it is in.
    void rightClickEntriesAreCounted() {
        HttpStub stub;
        QVERIFY(stub.isListening());
        TelemetryService service(testConfig(stub, dataDir_.filePath(QStringLiteral("right-click"))),
                                 [] { return kInstallId; }, true);
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit(QStringLiteral("a.txt"), "a\n", QStringLiteral("base")).ok());
        QVERIFY(repo.writeAndCommit(QStringLiteral("a.txt"), "b\n", QStringLiteral("second")).ok());
        const gitbolt::git::GitProcess git(repo.path().toStdString());
        const auto run = [&](const std::vector<std::string>& args) {
            const auto out = git.run(args);
            return out.ok() && out.value().success() ? QString::fromStdString(out.value().stdoutData).trimmed()
                                                     : QStringLiteral("failed");
        };
        QCOMPARE(run({"branch", "feature"}), QString());

        gitbolt::ui::MainWindow window;
        window.setTelemetry(&service, Gate::Active);
        service.start();
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        window.openRepositoryAtPath(repo.path());
        auto* svc = window.findChild<GitService*>();
        auto* commit = window.findChild<QAction*>(QStringLiteral("act.commit"));
        QVERIFY(svc && commit);
        QTRY_VERIFY_WITH_TIMEOUT(svc->isOpen() && !svc->openPending() && commit->isEnabled(), 15000);

        // The log's Revert This Commit…: its dialog opens, modeless.
        auto* log = window.findChild<QTableView*>(QStringLiteral("log.view"));
        QVERIFY(log);
        QTRY_VERIFY_WITH_TIMEOUT(log->model()->rowCount() > 0, 10000);
        QVERIFY(clickInRowMenu(log, log->model()->index(0, 1), QStringLiteral("log.revert-commit")));
        auto* pick = window.findChild<gitbolt::dialogs::CherryPickDialog*>();
        QVERIFY(pick && pick->isVisible());
        pick->close();

        // The branch tree's Checkout 'feature'.
        auto* branches = window.findChild<QTreeView*>(QStringLiteral("branchTree.view"));
        QVERIFY(branches);
        QModelIndex feature;
        QTRY_VERIFY((feature = rowOf(branches->model(), QStringLiteral("feature"))).isValid());
        QVERIFY(clickInRowMenu(branches, feature, QStringLiteral("branches.checkout")));
        QTRY_COMPARE_WITH_TIMEOUT(run({"branch", "--show-current"}), QStringLiteral("feature"), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(commit->isEnabled(), 15000);

        // The diff's menu: Qt's Select All under GitBolt's entries.
        auto* diff = window.findChild<QPlainTextEdit*>(QStringLiteral("diff.unifiedEditor"));
        QVERIFY(diff);
        diff->setPlainText(QStringLiteral("text"));
        QVERIFY(clickInMenu([&] { emit diff->customContextMenuRequested(diff->rect().center()); },
                            QStringLiteral("select-all")));
        QCOMPARE(diff->textCursor().selectedText(), QStringLiteral("text"));

        // A menu-bar action in a right-click menu.
        auto* home = window.findChild<QAction*>(QStringLiteral("file.home"));
        QVERIFY(home);
        QMenu shared(&window);
        shared.addAction(home);
        QVERIFY(clickInMenu([&] { shared.exec(window.mapToGlobal(window.rect().center())); },
                            QStringLiteral("file.home")));

        // File › Recent Repositories, whose entries count as one name.
        auto* recent = window.findChild<QAction*>(QStringLiteral("file.recent.0"));
        auto* recentMenu = recent ? qobject_cast<QMenu*>(recent->parent()) : nullptr;
        QVERIFY(recentMenu);
        QVERIFY(clickInMenu([&] { recentMenu->exec(window.mapToGlobal(window.rect().center())); },
                            QStringLiteral("file.recent.0")));
        QTRY_VERIFY_WITH_TIMEOUT(svc->isOpen() && !svc->openPending() && commit->isEnabled(), 15000);

        const QHash<QString, int> expected{{QStringLiteral("log.revert-commit"), 1},
                                           {QStringLiteral("branches.checkout"), 1},
                                           {QStringLiteral("file.home"), 1},
                                           {QStringLiteral("file.recent"), 1}};
        QTRY_COMPARE(commandCounts(stub), expected);
        QTest::qWait(500);   // two count windows: nothing else, nothing twice
        QCOMPARE(commandCounts(stub), expected);
    }

    // A toolbar's » menu is not a right-click menu: the Worktrees
    // window's toolbar (made on demand, never walked) stays uncounted
    // however narrow the window; its right-click Remove counts by name.
    void toolbarOverflowMenuIsNotCounted() {
        HttpStub stub;
        QVERIFY(stub.isListening());
        TelemetryService service(testConfig(stub, dataDir_.filePath(QStringLiteral("overflow"))),
                                 [] { return kInstallId; }, true);
        gitbolt::ui::MainWindow window;
        window.setTelemetry(&service, Gate::Active);
        service.start();

        gitbolt::widgets::WorktreeWidget panel;
        gitbolt::git::WorktreeInfo worktree;
        worktree.name = worktree.path = "wt";
        panel.setWorktrees({worktree});
        panel.resize(60, 200);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        auto* more = panel.findChild<QToolButton*>(QStringLiteral("qt_toolbar_ext_button"));
        QVERIFY(more && more->isVisible());
        QVERIFY(clickInMenu([&] { QTest::mouseClick(more, Qt::LeftButton); },
                            QStringLiteral("worktree.remove")));
        panel.findChild<QAction*>(QStringLiteral("worktree.remove"))->trigger();

        auto* table = panel.findChild<QTableView*>();
        QVERIFY(table);
        QVERIFY(clickInRowMenu(table, table->model()->index(0, 0), QStringLiteral("worktree-list.remove")));

        const QHash<QString, int> expected{{QStringLiteral("worktree-list.remove"), 1}};
        QTRY_COMPARE(commandCounts(stub), expected);
        QTest::qWait(500);   // two count windows: nothing else
        QCOMPARE(commandCounts(stub), expected);
    }

private:
    QTemporaryDir settingsDir_;
    QTemporaryDir dataDir_;
};

QTEST_MAIN(TestTelemetrySwitch)
#include "TestTelemetrySwitch.moc"
