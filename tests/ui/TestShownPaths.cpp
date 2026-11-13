// Tests for paths and HEAD as the window shows them: native separators
// (Windows CI checks them for real), the "dubious ownership" refusal and
// Repository Statistics' branch.

#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QScopeGuard>
#include <QSettings>
#include <QSignalSpy>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTreeWidget>

#include <functional>

#include <git2.h>

#include "../TestRepoHelper.h"
#include "conf/SettingsService.h"
#include "dialogs/CloneDialog.h"
#include "git/GitProcessLog.h"
#include "services/GitService.h"
#include "ui/DashboardView.h"
#include "ui/MainWindow.h"

using gitbolt::conf::SettingsService;
using gitbolt::services::GitService;
using gitbolt::ui::MainWindow;

#ifdef GITBOLT_LIBGIT2_BUNDLED
// libgit2's test hook (src/util/fs_path.h), in the static library: every
// file is owned by `owner` (a git_fs_path_owner_t) until it is 0 again.
extern "C" void git_fs_path__set_owner(int owner);
#endif

namespace {

// Calls `read` with the next modal dialog to come up, then closes it.
class NextModal {
public:
    explicit NextModal(std::function<void(QDialog*)> read) {
        timer_.setInterval(20);
        QObject::connect(&timer_, &QTimer::timeout, [this, read = std::move(read)] {
            auto* modal = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!modal || !modal->isVisible())
                return;
            timer_.stop();
            read(modal);
            done = true;
            modal->reject();
        });
        timer_.start();
    }
    bool done = false;

private:
    QTimer timer_;
};

bool writeFile(const QString& path, const QByteArray& data) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(data) == data.size();
}

} // namespace

class TestShownPaths : public QObject {
    Q_OBJECT

private slots:
    // Settings, and git's global config, in a folder of the test's own.
    void initTestCase() {
        QVERIFY(settingsDir_.isValid());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir_.path());
        const QString global = settingsDir_.filePath(QStringLiteral("gitconfig"));
        QVERIFY(writeFile(global, "[user]\n\tname = T\n\temail = t@example.com\n"));
        qputenv("GIT_CONFIG_GLOBAL", QFile::encodeName(global));
        qputenv("GIT_CONFIG_NOSYSTEM", "1");
    }

    void theWindowShowsTheRepositorysPath() {
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit(QStringLiteral("a.txt"), "a\n", QStringLiteral("first")).ok());
        MainWindow window;
        QSignalSpy notes(window.statusBar(), &QStatusBar::messageChanged);
        QVERIFY(openIn(window, repo.path()));

        const QString shown = QDir::toNativeSeparators(repo.path());
        QCOMPARE(window.windowTitle(), QDir(repo.path()).dirName() + QStringLiteral("  -  ") + shown);
        bool labelled = false;
        for (const QLabel* label : window.statusBar()->findChildren<QLabel*>())
            labelled = labelled || label->text() == shown;
        QVERIFY2(labelled, qPrintable(shown));
        QStringList said;
        for (const QList<QVariant>& note : std::as_const(notes))
            said << note.at(0).toString();
        QVERIFY2(said.contains(QStringLiteral("Opened: ") + shown), qPrintable(said.join(u'|')));
    }

    void theRecentListShowsPathsNatively_data() {
        QTest::addColumn<int>("strategy");
        QTest::newRow("whole") << int(SettingsService::RecentShortening::None);
        QTest::newRow("middle left out") << int(SettingsService::RecentShortening::MiddleEllipsis);
        QTest::newRow("last folder") << int(SettingsService::RecentShortening::SignificantDir);
    }

    // Shortened too: on Windows it split the path at '\' while QFileInfo
    // gave it '/', and never shortened it.
    void theRecentListShowsPathsNatively() {
        QFETCH(int, strategy);
        QTemporaryDir scratch;
        QVERIFY(scratch.isValid());
        const QString folder = scratch.filePath(QStringLiteral("one/two/three/four/five/six"));
        QVERIFY(QDir().mkpath(folder));
        QVERIFY(gitbolt::git::Repository::init(folder.toStdString()).ok());
        SettingsService settings;
        settings.clearRecentRepositories();
        settings.setRecentShorteningStrategy(SettingsService::RecentShortening(strategy));
        settings.addRecentRepository(folder);
        const auto clear = qScopeGuard([&settings] { settings.clearRecentRepositories(); });

        gitbolt::ui::DashboardView view(&settings);
        auto* list = view.findChild<QTreeWidget*>();
        QVERIFY(list);
        QCOMPARE(list->topLevelItemCount(), 1);
        const QTreeWidgetItem* row = list->topLevelItem(0);
        const QString listed = row->data(0, Qt::UserRole).toString();
        const QString whole = QDir::toNativeSeparators(QDir::cleanPath(folder));
        QCOMPARE(listed, whole);
        QCOMPARE(row->toolTip(6), whole);

        // The first two folders, as Qt spells them, and the last ones.
        const QStringList parts = QDir::cleanPath(folder).split(u'/', Qt::SkipEmptyParts);
        const QString lead = folder.startsWith(u'/') ? QStringLiteral("/") : QString();
        QString expected;
        switch (SettingsService::RecentShortening(strategy)) {
        case SettingsService::RecentShortening::None:
            expected = whole;
            break;
        case SettingsService::RecentShortening::MiddleEllipsis:
            expected = QDir::toNativeSeparators(lead + parts.mid(0, 2).join(u'/')
                                                + QStringLiteral("/…/five/six"));
            break;
        case SettingsService::RecentShortening::SignificantDir:
            expected = QDir::toNativeSeparators(QStringLiteral("…/six"));
            break;
        }
        QCOMPARE(row->text(6), expected);
        QTRY_VERIFY_WITH_TIMEOUT(view.findChildren<QFutureWatcherBase*>().isEmpty(), 15000);
    }

    void cloneToShowsTheSystemsSeparators() {
        gitbolt::dialogs::CloneDialog dialog;
        auto* url = dialog.findChild<QLineEdit*>(QStringLiteral("clone.url"));
        auto* path = dialog.findChild<QLineEdit*>(QStringLiteral("clone.path"));
        QVERIFY(url && path);
        QVERIFY(!path->placeholderText().isEmpty());
        QCOMPARE(path->placeholderText(), QDir::toNativeSeparators(path->placeholderText()));
        url->setText(QStringLiteral("https://example.com/org/project.git"));
        QCOMPARE(path->text(),
                 QDir::toNativeSeparators(path->placeholderText() + QStringLiteral("/project")));
    }

    void anotherUsersRepositoryIsRefusedWithTheFix() {
#ifndef GITBOLT_LIBGIT2_BUNDLED
        QSKIP("needs the bundled libgit2's test hook");
#else
        gitbolt::test::TestRepo repo;
        MainWindow window;
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QSignalSpy notes(window.statusBar(), &QStatusBar::messageChanged);

        constexpr int kOtherUser = 1 << 4;   // GIT_FS_PATH_OWNER_OTHER
        git_fs_path__set_owner(kOtherUser);
        const auto restore = qScopeGuard([] { git_fs_path__set_owner(0); });

        QString name, title, text, informative;
        bool plain = false, selectable = false, qts = false;
        NextModal box([&](QDialog* modal) {
            auto* message = qobject_cast<QMessageBox*>(modal);
            if (!message)
                return;
            name = message->objectName();
            title = message->windowTitle();
            text = message->text();
            informative = message->informativeText();
            // Qt's box, whose label is set so: not macOS's NSAlert.
            qts = message->testOption(QMessageBox::Option::DontUseNativeDialog);
            if (auto* label = message->findChild<QLabel*>(QStringLiteral("qt_msgbox_informativelabel"))) {
                plain = label->textFormat() == Qt::PlainText;
                selectable = label->textInteractionFlags().testFlag(Qt::TextSelectableByMouse);
            }
        });
        window.openRepositoryAtPath(repo.path());
        QTRY_VERIFY_WITH_TIMEOUT(box.done, 15000);

        QCOMPARE(name, QStringLiteral("dlg.openFailed"));
#ifndef Q_OS_MACOS   // where a message box has no title
        QCOMPARE(title, QStringLiteral("Repository Owned by Another User"));
#endif
        const QString intro = QStringLiteral(
            "If you trust it, add it to git's safe.directory list with this command, then open "
            "it again:\n\ngit config --global --add safe.directory ");
        QVERIFY2(informative.startsWith(intro), qPrintable(informative));
        QString folder = informative.mid(intro.size());
        if (folder.size() > 1 && (folder.front() == u'"' || folder.front() == u'\''))
            folder = folder.mid(1, folder.size() - 2);
        QCOMPARE(QFileInfo(folder).canonicalFilePath(), QFileInfo(repo.path()).canonicalFilePath());
        QVERIFY2(!folder.contains(u'\\'), qPrintable(folder));   // as libgit2 compares it
        QCOMPARE(text, QStringLiteral("The repository at %1 belongs to another user account, so "
                                      "GitBolt didn't open it.")
                           .arg(QDir::toNativeSeparators(folder)));
        QVERIFY(plain);
        QVERIFY(selectable);
        QVERIFY(qts);

        QStringList said;
        for (const QList<QVariant>& note : std::as_const(notes))
            said << note.at(0).toString();
        QVERIFY2(said.contains(QStringLiteral("Failed to open ")
                               + QDir::toNativeSeparators(repo.path())),
                 qPrintable(said.join(u'|')));
        QVERIFY(!window.findChild<GitService*>()->isOpen());
#endif
    }

    // The Git Command Log's "(in <folder>)", an argument's "%4" left as
    // it is: the folder took its place too.
    void theCommandLogShowsItsFolder() {
        const QString folder = QStringLiteral("/x/shown-paths/repo");
        gitbolt::git::GitProcessLog::instance().emitCommand(
            folder, {QStringLiteral("commit"), QStringLiteral("-m"), QStringLiteral("50%4 off")}, 0, 7);
        MainWindow window;
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto* log = window.findChild<QAction*>(QStringLiteral("tools.git-command-log"));
        QVERIFY(log && log->isEnabled());
        log->trigger();
        QDialog* dialog = nullptr;
        for (QDialog* candidate : window.findChildren<QDialog*>()) {
            if (candidate->windowTitle() == QLatin1String("Git Command Log"))
                dialog = candidate;
        }
        QVERIFY(dialog);
        const auto* text = dialog->findChild<QPlainTextEdit*>();
        QVERIFY(text);
        const QString line = QStringLiteral("$ git commit -m 50%4 off   (in %1)")
                                 .arg(QDir::toNativeSeparators(folder));
        QVERIFY2(text->toPlainText().contains(line), qPrintable(text->toPlainText()));
        dialog->close();
    }

    void statisticsNameTheBranchAsTheStatusBarDoes() {
        gitbolt::test::TestRepo repo;
        const QString branch = QString::fromStdString(repo.repo().unbornBranchName());
        QVERIFY(!branch.isEmpty());
        MainWindow window;
        QVERIFY(openIn(window, repo.path()));
        QCOMPARE(statisticsBranch(window), branch + QStringLiteral(" (no commits yet)"));

        QVERIFY(repo.writeAndCommit(QStringLiteral("a.txt"), "a\n", QStringLiteral("first")).ok());
        QCOMPARE(statisticsBranch(window), branch);

        git_oid head;
        QCOMPARE(git_reference_name_to_id(&head, repo.repo().raw(), "HEAD"), 0);
        QCOMPARE(git_repository_set_head_detached(repo.repo().raw(), &head), 0);
        QCOMPARE(statisticsBranch(window), QStringLiteral("(detached)"));
    }

private:
    // Opens `path` and returns the window's GitService once the
    // repository is on screen.
    static GitService* openIn(MainWindow& window, const QString& path) {
        window.show();
        if (!QTest::qWaitForWindowExposed(&window))
            return nullptr;
        window.openRepositoryAtPath(path);
        auto* svc = window.findChild<GitService*>();
        auto* commit = window.findChild<QAction*>(QStringLiteral("act.commit"));
        if (!svc || !commit)
            return nullptr;
        const bool open = QTest::qWaitFor([&] {
            return svc->isOpen() && !svc->openPending() && commit->isEnabled();
        }, 15000);
        return open ? svc : nullptr;
    }

    // Repository Statistics' "Current branch:".
    static QString statisticsBranch(MainWindow& window) {
        auto* statistics = window.findChild<QAction*>(QStringLiteral("plugins.statistics"));
        if (!statistics || !statistics->isEnabled())
            return QStringLiteral("<no Statistics>");
        QString shown;
        NextModal box([&shown](QDialog* modal) {
            if (modal->objectName() != QLatin1String("dlg.statistics"))
                return;
            if (auto* text = modal->findChild<QPlainTextEdit*>())
                shown = text->toPlainText();
        });
        statistics->trigger();
        if (!QTest::qWaitFor([&box] { return box.done; }, 10000))
            return QStringLiteral("<no box>");
        const QString key = QStringLiteral("Current branch:");
        for (const QString& line : shown.split(u'\n')) {
            if (line.trimmed().startsWith(key))
                return line.trimmed().mid(key.size()).trimmed();
        }
        return QStringLiteral("<no line in: %1>").arg(shown);
    }

    QTemporaryDir settingsDir_;
};

QTEST_MAIN(TestShownPaths)
#include "TestShownPaths.moc"
