//
// TestCommitDialog — the Commit dialog against a real repository:
//
//   - Amend is off, with git's reason as its tooltip, while a merge, a
//     cherry-pick or a revert waits for its commit (an amend there
//     flattened the merge, or folded the pick into the commit before
//     it), and on again once the operation is over;
//   - the message starts as the one git prepared for a merge,
//     cherry-pick or revert waiting for its commit (MERGE_MSG, without
//     its comment lines), as a terminal's `git commit` does;
//   - a commit that a hook turns down shows what the hook said, and the
//     dialog stays open with its message;
//   - while the commit runs, nothing stages, unstages or commits, the
//     button clicked says "Committing…", and Commit & Push still pushes
//     once it's in; the file lists and the diff stay live to review;
//
// Settings live in a folder of the test's own (QSettings::setPath), and
// git's global configuration is an empty file there: a developer's
// core.hooksPath or commit.gpgSign stays out of it.
//

#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QFile>
#include <QLabel>
#include <QPlainTextEdit>
#include <QFileInfo>
#include <QPushButton>
#include <QScopeGuard>
#include <QSettings>
#include <QShortcut>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include "../TestRepoHelper.h"
#include "dialogs/CommitDialog.h"
#include "widgets/DiffViewerWidget.h"
#include "git/GitProcess.h"
#include "services/GitService.h"

using gitbolt::dialogs::CommitDialog;
using gitbolt::services::GitService;

namespace {

bool succeeded(const gitbolt::git::Result<gitbolt::git::ProcessOutput>& r) {
    return r.ok() && r.value().success();
}

// Trimmed stdout, or "<failed>" when git failed.
QString output(const gitbolt::git::Result<gitbolt::git::ProcessOutput>& r) {
    return succeeded(r) ? QString::fromStdString(r.value().stdoutData).trimmed()
                        : QStringLiteral("<failed>");
}

template <typename T>
T* child(const QWidget& parent, const char* name) {
    return parent.findChild<T*>(QString::fromLatin1(name));
}

} // namespace

class TestCommitDialog : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        QVERIFY(settingsDir_.isValid());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir_.path());
        const QString global = settingsDir_.filePath(QStringLiteral("gitconfig"));
        QFile empty(global);
        QVERIFY(empty.open(QIODevice::WriteOnly));
        qputenv("GIT_CONFIG_GLOBAL", QFile::encodeName(global));
        qputenv("GIT_CONFIG_NOSYSTEM", "1");
    }

    void amendIsOffWhileAnOperationWaitsForItsCommit_data() {
        QTest::addColumn<QString>("operation");
        QTest::newRow("merge") << QStringLiteral("merge");
        QTest::newRow("cherry-pick") << QStringLiteral("cherry-pick");
        QTest::newRow("revert") << QStringLiteral("revert");
    }

    void amendIsOffWhileAnOperationWaitsForItsCommit() {
        QFETCH(QString, operation);
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit(QStringLiteral("a.txt"), "base\n", QStringLiteral("base")).ok());
        gitbolt::git::GitProcess git(repo.path().toStdString());
        QVERIFY(succeeded(git.run({"config", "core.autocrlf", "false"})));
        QVERIFY(succeeded(git.run({"checkout", "-q", "-b", "side"})));
        repo.writeFile(QStringLiteral("a.txt"), "side\n");
        QVERIFY(succeeded(git.run({"commit", "-q", "-am", "side"})));
        QVERIFY(succeeded(git.run({"checkout", "-q", "-"})));
        repo.writeFile(QStringLiteral("a.txt"), "main\n");
        QVERIFY(succeeded(git.run({"commit", "-q", "-am", "main"})));

        GitService svc;
        QVERIFY(svc.openRepository(repo.path()));
        CommitDialog dialog(&svc, nullptr);
        auto* amend = child<QCheckBox>(dialog, "commit.amendCheck");
        QVERIFY(amend);
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        QVERIFY(amend->isEnabled());
        amend->setChecked(true);

        // git, in a terminal say, stops on the conflict in a.txt.
        const std::string command = operation.toStdString();
        if (operation == QLatin1String("revert"))
            QVERIFY(!succeeded(git.run({"revert", "--no-edit", "side"})));
        else
            QVERIFY(!succeeded(git.run({command, "side"})));
        svc.refreshStatus();
        QTRY_COMPARE(amend->toolTip(),
                     QStringLiteral("You are in the middle of a %1 -- cannot amend.").arg(operation));
        QVERIFY(!amend->isEnabled());
        QVERIFY(!amend->isChecked());

        QVERIFY(succeeded(git.run({command, "--abort"})));
        svc.refreshStatus();
        QTRY_VERIFY(amend->isEnabled());
        QCOMPARE(amend->toolTip(), QString());
    }

    // A merge, cherry-pick or revert stopped on a conflict: the message
    // starts as the one git prepared for it (MERGE_MSG, its comment
    // lines left out), as a terminal's `git commit` does. The dialog's
    // commit (`git commit -F -`) took the typed words alone, and a
    // revert's "This reverts commit <id>." was gone: Continue on a
    // revert of several commits then said git never reverted the one
    // just committed, and advised Abort.
    void messageStartsAsTheOneGitPrepared_data() {
        QTest::addColumn<QString>("operation");
        QTest::newRow("merge") << QStringLiteral("merge");
        QTest::newRow("cherry-pick") << QStringLiteral("cherry-pick");
        QTest::newRow("revert") << QStringLiteral("revert");
    }

    void messageStartsAsTheOneGitPrepared() {
        QFETCH(QString, operation);
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit(QStringLiteral("a.txt"), "base\n", QStringLiteral("base")).ok());
        gitbolt::git::GitProcess git(repo.path().toStdString());
        QVERIFY(succeeded(git.run({"config", "core.autocrlf", "false"})));
        QVERIFY(succeeded(git.run({"checkout", "-q", "-b", "side"})));
        repo.writeFile(QStringLiteral("a.txt"), "side\n");
        QVERIFY(succeeded(git.run({"commit", "-q", "-am", "side change"})));
        const QString side = output(git.run({"rev-parse", "side"}));
        QVERIFY(succeeded(git.run({"checkout", "-q", "-"})));
        repo.writeFile(QStringLiteral("a.txt"), "main\n");
        QVERIFY(succeeded(git.run({"commit", "-q", "-am", "main change"})));
        const std::string command = operation.toStdString();
        if (operation == QLatin1String("revert")) {
            // A revert of "side change", picked onto main first.
            QVERIFY(succeeded(git.run({"reset", "-q", "--hard", "side"})));
            repo.writeFile(QStringLiteral("a.txt"), "later\n");
            QVERIFY(succeeded(git.run({"commit", "-q", "-am", "later change"})));
            QVERIFY(!succeeded(git.run({"revert", "--no-edit", "side"})));
        } else {
            QVERIFY(!succeeded(git.run({command, "side"})));
        }
        // git's own, with the conflict noted in comment lines.
        QFile prepared(repo.path() + QStringLiteral("/.git/MERGE_MSG"));
        QVERIFY(prepared.open(QIODevice::ReadOnly));
        QVERIFY(prepared.readAll().contains("\n#"));

        GitService svc;
        QVERIFY(svc.openRepository(repo.path()));
        CommitDialog dialog(&svc, nullptr);
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        auto* message = child<QPlainTextEdit>(dialog, "commit.message");
        auto* commit = child<QPushButton>(dialog, "commit.commitBtn");
        QVERIFY(message && commit);
        const QString expected =
            operation == QLatin1String("merge") ? QStringLiteral("Merge branch 'side'")
            : operation == QLatin1String("cherry-pick")
                ? QStringLiteral("side change")
                : QStringLiteral("Revert \"side change\"\n\nThis reverts commit %1.").arg(side);
        QTRY_COMPARE(message->toPlainText(), expected);

        // Resolved and committed from the dialog, with that message.
        repo.writeFile(QStringLiteral("a.txt"), "resolved\n");
        QVERIFY(succeeded(git.run({"add", "a.txt"})));
        svc.refreshStatus();
        QTRY_VERIFY(commit->isEnabled());
        commit->click();
        QTRY_VERIFY_WITH_TIMEOUT(!dialog.isVisible(), 15000);
        QCOMPARE(output(git.run({"log", "-1", "--format=%B"})), expected);
        QCOMPARE(repo.repo().state(), gitbolt::git::RepoState::None);

        // Nothing prepared, nothing filled in.
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        QTest::qWait(200);
        QCOMPARE(message->toPlainText(), QString());
    }

    // The commit goes through git, which runs the repository's hooks;
    // a pre-commit's refusal is what the dialog shows.
    void hookRefusalShowsInTheDialog() {
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit(QStringLiteral("a.txt"), "a\n", QStringLiteral("a")).ok());
        gitbolt::git::GitProcess git(repo.path().toStdString());
        const QString head = output(git.run({"rev-parse", "HEAD"}));
        QDir().mkpath(repo.path() + QStringLiteral("/.git/hooks"));
        QFile hook(repo.path() + QStringLiteral("/.git/hooks/pre-commit"));
        QVERIFY(hook.open(QIODevice::WriteOnly));
        hook.write("#!/bin/sh\necho 'pre-commit: no secrets, please' >&2\nexit 1\n");
        hook.close();
        QVERIFY(hook.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        repo.writeFile(QStringLiteral("b.txt"), "b\n");
        QVERIFY(repo.stageFile(QStringLiteral("b.txt")).ok());

        GitService svc;
        QVERIFY(svc.openRepository(repo.path()));
        CommitDialog dialog(&svc, nullptr);
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        auto* message = child<QPlainTextEdit>(dialog, "commit.message");
        auto* commit = child<QPushButton>(dialog, "commit.commitBtn");
        auto* error = child<QLabel>(dialog, "commit.error");
        QVERIFY(message && commit && error);
        message->setPlainText(QStringLiteral("refused"));
        commit->click();

        QTRY_VERIFY_WITH_TIMEOUT(error->isVisible(), 15000);
        QVERIFY2(error->text().contains(QStringLiteral("pre-commit: no secrets, please")),
                 qPrintable(error->text()));
        QVERIFY(dialog.isVisible());
        QVERIFY(commit->isEnabled());
        QCOMPARE(message->toPlainText(), QStringLiteral("refused"));
        QCOMPARE(output(git.run({"rev-parse", "HEAD"})), head);
    }

    // git commit reads the index again after the pre-commit hook, and
    // the dialog's staging stayed live while it ran: a file staged then
    // went into the commit without the hook seeing it, and Unstage All
    // made it empty. A second Commit (Ctrl+Return) started a second git
    // commit. Everything that stages or commits is off until it's done.
    void stagingIsLockedWhileTheCommitRuns() {
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit(QStringLiteral("a.txt"), "a\n", QStringLiteral("a")).ok());
        gitbolt::git::GitProcess git(repo.path().toStdString());
        QVERIFY(holdCommitsAtTheHook(repo));
        repo.writeFile(QStringLiteral("b.txt"), "b\n");
        QVERIFY(repo.stageFile(QStringLiteral("b.txt")).ok());
        repo.writeFile(QStringLiteral("secret.env"), "token\n");

        GitService svc;
        QVERIFY(svc.openRepository(repo.path()));
        QSignalSpy completed(&svc, &GitService::commitComplete);
        CommitDialog dialog(&svc, nullptr);
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        auto* message = child<QPlainTextEdit>(dialog, "commit.message");
        auto* commit = child<QPushButton>(dialog, "commit.commitBtn");
        QVERIFY(message && commit);
        QTRY_VERIFY(child<QPushButton>(dialog, "commit.stageAllBtn")->isEnabled());
        message->setPlainText(QStringLiteral("add b"));
        commit->click();
        QTRY_VERIFY_WITH_TIMEOUT(hookStarted(repo), 15000);

        for (const char* name : {"commit.stageAllBtn", "commit.unstageAllBtn",
                                 "commit.stageBtn", "commit.unstageBtn",
                                 "commit.discardBtn", "commit.commitBtn",
                                 "commit.commitPushBtn", "commit.amendCheck"})
            QVERIFY2(!child<QWidget>(dialog, name)->isEnabled(), name);
        // The lists and the diff stay live, to scroll and review: they
        // were disabled outright, the diff unscrollable while the hook
        // ran. What they do that stages is off: a double-click or Enter
        // on a row, and the diff's hunk and line actions.
        auto* unstaged = child<QAbstractItemView>(dialog, "commit.unstagedList");
        auto* diff = dialog.findChild<gitbolt::widgets::DiffViewerWidget*>();
        QVERIFY(unstaged && diff);
        for (QWidget* live : {static_cast<QWidget*>(unstaged),
                              child<QWidget>(dialog, "commit.stagedList"),
                              static_cast<QWidget*>(diff)})
            QVERIFY2(live->isEnabled(), qPrintable(live->objectName()));
        QVERIFY(diff->stagingLocked());
        QCOMPARE(unstaged->model()->rowCount(), 1);
        const QModelIndex secret = unstaged->model()->index(0, 0);
        unstaged->setCurrentIndex(secret);
        QVERIFY(!child<QWidget>(dialog, "commit.stageBtn")->isEnabled());
        QVERIFY(!child<QWidget>(dialog, "commit.discardBtn")->isEnabled());
        emit unstaged->activated(secret);
        QTest::qWait(200);
        QCOMPARE(output(git.run({"diff", "--cached", "--name-only"})), QStringLiteral("b.txt"));
        // And it says so where it shows, greyed out or not: a tooltip on
        // the disabled button was all there was while the hook ran.
        QCOMPARE(commit->text(), QStringLiteral("Committing…"));
        // Ctrl+Return, as its shortcut would.
        auto* shortcut = dialog.findChild<QShortcut*>();
        QVERIFY(shortcut);
        QVERIFY(!shortcut->isEnabled());
        QMetaObject::invokeMethod(&dialog, "onCommitClicked");

        release(repo);
        QTRY_VERIFY_WITH_TIMEOUT(!dialog.isVisible(), 15000);
        QTest::qWait(200);
        QCOMPARE(completed.count(), 1);
        QCOMPARE(output(git.run({"log", "--format=%s"})), QStringLiteral("add b\na"));
        QCOMPARE(output(git.run({"show", "--name-only", "--format=", "HEAD"})),
                 QStringLiteral("b.txt"));
        QVERIFY(child<QPushButton>(dialog, "commit.stageAllBtn")->isEnabled());
        QVERIFY(commit->isEnabled());
        QCOMPARE(commit->text(), QStringLiteral("Commit"));
        QVERIFY(!diff->stagingLocked());
    }

    // Commit & Push pushes once the commit is in. A stage that failed
    // meanwhile ended the wait (the commit's outcome was dropped, and
    // the push with it), and so did the same repository opened again
    // (from Recent, or handed over by a second launch), whose commit
    // still reports.
    void commitAndPushStillPushes_data() {
        QTest::addColumn<QString>("meanwhile");
        QTest::newRow("a stage failed") << QStringLiteral("stage");
        QTest::newRow("the repository opened again") << QStringLiteral("reopen");
    }

    void commitAndPushStillPushes() {
        QFETCH(QString, meanwhile);
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit(QStringLiteral("a.txt"), "a\n", QStringLiteral("a")).ok());
        QVERIFY(holdCommitsAtTheHook(repo));
        repo.writeFile(QStringLiteral("b.txt"), "b\n");
        QVERIFY(repo.stageFile(QStringLiteral("b.txt")).ok());

        GitService svc;
        QVERIFY(svc.openRepository(repo.path()));
        CommitDialog dialog(&svc, nullptr);
        QSignalSpy pushes(&dialog, &CommitDialog::pushAfterCommitRequested);
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        child<QPlainTextEdit>(dialog, "commit.message")->setPlainText(QStringLiteral("add b"));
        child<QPushButton>(dialog, "commit.commitPushBtn")->click();
        QTRY_VERIFY_WITH_TIMEOUT(hookStarted(repo), 15000);
        QCOMPARE(child<QPushButton>(dialog, "commit.commitPushBtn")->text(),
                 QStringLiteral("Committing…"));

        if (meanwhile == QLatin1String("stage"))
            emit svc.operationFailed(QStringLiteral("stage"), QStringLiteral("index.lock exists"));
        else
            QVERIFY(svc.openRepository(repo.path()));
        QVERIFY(!child<QPushButton>(dialog, "commit.commitBtn")->isEnabled());

        release(repo);
        QTRY_COMPARE_WITH_TIMEOUT(pushes.count(), 1, 15000);
    }

    // Away to another repository and back while the hook runs: the
    // other one's open ended the dialog's wait for its commit (that
    // commit reports nowhere then), and nothing brought it back, so the
    // dialog unlocked with the commit still running. A file staged then
    // went into it, the hook never seeing it. The dialog stays locked
    // while a step runs on its repository, whoever's, and the service
    // turns staging away meanwhile too.
    void awayAndBackDuringTheCommitStaysLocked() {
        gitbolt::test::TestRepo repo;
        gitbolt::test::TestRepo other;
        QVERIFY(repo.writeAndCommit(QStringLiteral("a.txt"), "a\n", QStringLiteral("a")).ok());
        QVERIFY(other.writeAndCommit(QStringLiteral("o.txt"), "o\n", QStringLiteral("o")).ok());
        gitbolt::git::GitProcess git(repo.path().toStdString());
        QVERIFY(holdCommitsAtTheHook(repo));
        repo.writeFile(QStringLiteral("x.txt"), "x\n");
        QVERIFY(repo.stageFile(QStringLiteral("x.txt")).ok());
        repo.writeFile(QStringLiteral("y.txt"), "y\n");

        GitService svc;
        QVERIFY(svc.openRepository(repo.path()));
        QSignalSpy completed(&svc, &GitService::commitComplete);
        CommitDialog dialog(&svc, nullptr);
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        const auto release = qScopeGuard([&] { TestCommitDialog::release(repo); });
        QTRY_VERIFY(child<QPushButton>(dialog, "commit.stageAllBtn")->isEnabled());
        child<QPlainTextEdit>(dialog, "commit.message")->setPlainText(QStringLiteral("only-x"));
        child<QPushButton>(dialog, "commit.commitBtn")->click();
        QTRY_VERIFY_WITH_TIMEOUT(hookStarted(repo), 15000);

        QVERIFY(svc.openRepository(other.path()));
        QVERIFY(svc.openRepository(repo.path()));
        QVERIFY(svc.rebaseStepRunning());
        for (const char* name : {"commit.stageAllBtn", "commit.unstageAllBtn",
                                 "commit.commitBtn", "commit.commitPushBtn",
                                 "commit.amendCheck"})
            QVERIFY2(!child<QWidget>(dialog, name)->isEnabled(), name);
        QSignalSpy failed(&svc, &GitService::operationFailed);
        svc.stageAll();
        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed.first().at(0).toString(), QStringLiteral("stage all"));

        TestCommitDialog::release(repo);
        QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 15000);
        QVERIFY2(completed.first().at(0).toBool(), qPrintable(completed.first().at(1).toString()));
        QCOMPARE(output(git.run({"show", "--name-only", "--format=%s", "HEAD"})),
                 QStringLiteral("only-x\n\nx.txt"));
        // Over, and the dialog's own again.
        QTRY_VERIFY(child<QPushButton>(dialog, "commit.stageAllBtn")->isEnabled());
        QVERIFY(child<QPushButton>(dialog, "commit.commitBtn")->isEnabled());
    }

private:
    // A pre-commit hook that notes it started and waits for release().
    static bool holdCommitsAtTheHook(gitbolt::test::TestRepo& repo) {
        QDir().mkpath(repo.path() + QStringLiteral("/.git/hooks"));
        QFile hook(repo.path() + QStringLiteral("/.git/hooks/pre-commit"));
        if (!hook.open(QIODevice::WriteOnly))
            return false;
        hook.write("#!/bin/sh\ntouch .git/hook-started\n"
                   "while [ ! -f .git/go ]; do sleep 0.05; done\n");
        hook.close();
        return hook.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    }
    static bool hookStarted(const gitbolt::test::TestRepo& repo) {
        return QFileInfo::exists(repo.path() + QStringLiteral("/.git/hook-started"));
    }
    static void release(gitbolt::test::TestRepo& repo) {
        repo.writeFile(QStringLiteral(".git/go"), QByteArray());
    }

    QTemporaryDir settingsDir_;
};

QTEST_MAIN(TestCommitDialog)
#include "TestCommitDialog.moc"
