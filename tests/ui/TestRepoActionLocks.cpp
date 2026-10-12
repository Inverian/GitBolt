//
// TestRepoActionLocks — a real MainWindow (off screen) while git works on
// its repository, and what it then lets the user do and tells them:
//
//   - while git commit's hooks run, the repository's actions are off,
//     the toolbar's branch switch and the sidebar's included: a switch
//     meanwhile made the commit land on the other branch;
//   - "Checkout as local branch" and a submodule's Update / Init are
//     local ops, with the busy UI and one at a time: they ran on a worker
//     with neither, and a second one met the first one's index.lock;
//   - a cherry-pick of several commits paused after a commit made during
//     it: Abort says the commits stay, as git doesn't rewind past one,
//     and its outcome says the same (it said "Cherry-pick aborted.");
//   - Skip at a reword stop whose new message a hook turned down says
//     the commit stays, under its old message (it said its changes were
//     left out);
//   - a local op that outlives a remote op started before it says again
//     what it does once the remote op's outcome has been up a while;
//   - the sidebar's New Branch is off while a local op runs, rather than
//     asking for a name it then threw away.
//
// Settings live in a folder of the test's own (QSettings::setPath), and
// git's global configuration is a file there with an identity and
// nothing else, so a developer's hooks or signing stay out of it.
//

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScopeGuard>
#include <QSettings>
#include <QSignalSpy>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include "../TestRepoHelper.h"
#include "git/GitProcess.h"
#include "git/Rebase.h"
#include "services/GitService.h"
#include "ui/MainWindow.h"
#include "widgets/BranchTreeWidget.h"
#include "widgets/RepoOperationBar.h"
#include "widgets/SubmoduleWidget.h"

using gitbolt::services::GitService;
using gitbolt::ui::MainWindow;

namespace {

bool succeeded(const gitbolt::git::Result<gitbolt::git::ProcessOutput>& r) {
    return r.ok() && r.value().success();
}

QString output(const gitbolt::git::Result<gitbolt::git::ProcessOutput>& r) {
    return succeeded(r) ? QString::fromStdString(r.value().stdoutData).trimmed()
                        : QStringLiteral("<failed>");
}

// The branch checked out (TestRepo's first one is libgit2's default).
std::string branchOf(const gitbolt::git::GitProcess& git) {
    return output(git.run({"branch", "--show-current"})).toStdString();
}

bool writeFile(const QString& path, const QByteArray& data) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return file.write(data) == data.size();
}

bool writeHook(const QString& repo, const char* name, const QByteArray& body) {
    const QString path = repo + QStringLiteral("/.git/hooks/") + QLatin1String(name);
    return writeFile(path, "#!/bin/sh\n" + body)
        && QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
}

// Answers the next message box with `button`, noting what it said.
class BoxAnswerer {
public:
    explicit BoxAnswerer(QMessageBox::StandardButton button) {
        timer_.setInterval(20);
        QObject::connect(&timer_, &QTimer::timeout, [this, button] {
            auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            if (!box)
                return;
            timer_.stop();
            text = box->text();
            if (QAbstractButton* b = box->button(button))
                b->click();
            else
                box->reject();
        });
        timer_.start();
    }
    QString text;

private:
    QTimer timer_;
};

} // namespace

class TestRepoActionLocks : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        QVERIFY(settingsDir_.isValid());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir_.path());
        const QString global = settingsDir_.filePath(QStringLiteral("gitconfig"));
        QVERIFY(writeFile(global, "[user]\n\tname = T\n\temail = t@example.com\n"
                                  "[commit]\n\tgpgSign = false\n"
                                  "[init]\n\tdefaultBranch = main\n"
                                  "[protocol \"file\"]\n\tallow = always\n"));
        qputenv("GIT_CONFIG_GLOBAL", QFile::encodeName(global));
        qputenv("GIT_CONFIG_NOSYSTEM", "1");
    }

    // git commit runs its pre-commit hook, then writes HEAD through the
    // branch HEAD names when it is done. The window stayed live: a
    // switch from the toolbar meanwhile moved HEAD to "other", which got
    // the commit, and main didn't. Now everything that acts on the
    // repository is off until the commit reports, and what gets past is
    // refused.
    void commitHookKeepsTheRepositoryActionsOff() {
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit(QStringLiteral("a.txt"), "a\n", QStringLiteral("base")).ok());
        gitbolt::git::GitProcess git(repo.path().toStdString());
        const std::string main = branchOf(git);
        QVERIFY(succeeded(git.run({"branch", "other"})));
        QVERIFY(writeHook(repo.path(), "pre-commit",
                          "touch .git/hook-started\n"
                          "while [ ! -f .git/go ]; do sleep 0.05; done\n"));
        repo.writeFile(QStringLiteral("b.txt"), "b\n");
        QVERIFY(succeeded(git.run({"add", "b.txt"})));

        MainWindow window;
        auto* svc = openIn(window, repo.path());
        QVERIFY(svc);
        auto* combo = window.findChild<QComboBox*>(QStringLiteral("toolbar.branchCombo"));
        auto* tree = window.findChild<gitbolt::widgets::BranchTreeWidget*>();
        auto* newBranch = window.findChild<QAction*>(QStringLiteral("branchTree.newBranch"));
        QVERIFY(combo && tree && newBranch);
        QVERIFY(combo->isEnabled() && newBranch->isEnabled());
        const auto release = qScopeGuard([&] { repo.writeFile(QStringLiteral(".git/go"), ""); });

        QSignalSpy done(svc, &GitService::commitComplete);
        svc->commitChanges(QStringLiteral("on main"));
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(repo.path() + QStringLiteral("/.git/hook-started")),
                                 15000);
        QVERIFY(!combo->isEnabled());
        QVERIFY(!newBranch->isEnabled());
        for (const char* name : {"act.commit", "act.pull", "act.push", "act.fetch", "act.stash"})
            QVERIFY2(!action(window, name)->isEnabled(), name);
        // The sidebar's checkout, and the service's own guard.
        emit tree->checkoutRequested(QStringLiteral("other"));
        QCOMPARE(window.statusBar()->currentMessage(),
                 QStringLiteral("Another operation is still running…"));
        QSignalSpy failed(svc, &GitService::operationFailed);
        QVERIFY(!svc->checkoutBranch(QStringLiteral("other")));
        QCOMPARE(failed.count(), 1);
        QCOMPARE(branchOf(git), main);
        // A local op (the sidebar's Merge) is turned away too.
        window.statusBar()->clearMessage();
        emit tree->mergeRequested(QStringLiteral("other"));
        QCOMPARE(window.statusBar()->currentMessage(),
                 QStringLiteral("Another operation is still running…"));
        QVERIFY(action(window, "act.commit") && !action(window, "act.commit")->isEnabled());

        repo.writeFile(QStringLiteral(".git/go"), "");
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 15000);
        QVERIFY2(done.first().at(0).toBool(), qPrintable(done.first().at(1).toString()));
        QCOMPARE(output(git.run({"log", "-1", "--format=%s", main})), QStringLiteral("on main"));
        QCOMPARE(output(git.run({"log", "-1", "--format=%s", "other"})), QStringLiteral("base"));
        QTRY_VERIFY(combo->isEnabled());
        QVERIFY(newBranch->isEnabled());
        QVERIFY(action(window, "act.commit")->isEnabled());
    }

    // The sidebar's "Checkout as local branch": git creates the branch,
    // tracking the remote one, and checks it out, as a local op. It ran
    // on a worker of the service's: nothing went busy or said anything,
    // and a second click ran a second git, which failed on index.lock
    // with git's advice to delete it. An existing branch is checked out
    // as it is.
    void checkoutAsLocalBranchIsALocalOp() {
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit(QStringLiteral("a.txt"), "a\n", QStringLiteral("base")).ok());
        QTemporaryDir originDir;
        QVERIFY(originDir.isValid());
        gitbolt::git::GitProcess git(repo.path().toStdString());
        QVERIFY(succeeded(git.run({"init", "-q", "--bare", originDir.path().toStdString()})));
        QVERIFY(succeeded(git.run({"remote", "add", "origin", originDir.path().toStdString()})));
        const std::string main = branchOf(git);
        QVERIFY(succeeded(git.run({"checkout", "-q", "-b", "feature/x"})));
        QVERIFY(succeeded(git.run({"commit", "-q", "--allow-empty", "-m", "on feature"})));
        const QString featureTip = output(git.run({"rev-parse", "HEAD"}));
        QVERIFY(succeeded(git.run({"push", "-q", "origin", main, "feature/x"})));
        QVERIFY(succeeded(git.run({"checkout", "-q", main})));
        QVERIFY(succeeded(git.run({"branch", "-q", "-D", "feature/x"})));
        QVERIFY(succeeded(git.run({"fetch", "-q", "origin"})));
        // A slow first checkout (LFS, a hook): a second click lands in it.
        QVERIFY(writeHook(repo.path(), "post-checkout", "sleep 2\n"));

        MainWindow window;
        auto* svc = openIn(window, repo.path());
        QVERIFY(svc);
        auto* tree = window.findChild<gitbolt::widgets::BranchTreeWidget*>();
        auto* label = window.findChild<QLabel*>(QStringLiteral("toolbar.opIndicator"));
        QVERIFY(tree && label);

        emit tree->checkoutRemoteRequested(QStringLiteral("origin/feature/x"));
        // Busy at once: the actions off, the label saying what runs.
        QVERIFY(!action(window, "act.commit")->isEnabled());
        QCOMPARE(label->text(), QStringLiteral("Checking out origin/feature/x…"));
        QSignalSpy failed(svc, &GitService::operationFailed);
        emit tree->checkoutRemoteRequested(QStringLiteral("origin/feature/x"));
        QCOMPARE(window.statusBar()->currentMessage(),
                 QStringLiteral("Another operation is still running…"));

        QTRY_VERIFY_WITH_TIMEOUT(action(window, "act.commit")->isEnabled(), 20000);
        QCOMPARE(failed.count(), 0);
        QCOMPARE(output(git.run({"branch", "--show-current"})), QStringLiteral("feature/x"));
        QCOMPARE(output(git.run({"rev-parse", "HEAD"})), featureTip);
        QCOMPARE(output(git.run({"rev-parse", "--abbrev-ref", "feature/x@{upstream}"})),
                 QStringLiteral("origin/feature/x"));
        QVERIFY(!QFileInfo::exists(repo.path() + QStringLiteral("/.git/index.lock")));

        // Already there: checked out as it is, not recreated or moved.
        QVERIFY(succeeded(git.run({"commit", "-q", "--allow-empty", "-m", "local only"})));
        const QString localTip = output(git.run({"rev-parse", "HEAD"}));
        QVERIFY(succeeded(git.run({"checkout", "-q", main})));
        emit tree->checkoutRemoteRequested(QStringLiteral("origin/feature/x"));
        QTRY_COMPARE_WITH_TIMEOUT(output(git.run({"branch", "--show-current"})),
                                  QStringLiteral("feature/x"), 10000);
        QCOMPARE(output(git.run({"rev-parse", "HEAD"})), localTip);
    }

    // Repository > Manage submodules…, a row's Update: a local op too, as
    // the dialog's Sync and Deinit are. It ran on a worker of the
    // service's, with no busy state.
    void submoduleUpdateIsALocalOp() {
        QTemporaryDir scratch;
        QVERIFY(scratch.isValid());
        const QString subUp = scratch.filePath(QStringLiteral("sub-upstream"));
        const QString super = scratch.filePath(QStringLiteral("super"));
        const auto git = [](const QString& dir, std::vector<std::string> args) {
            return succeeded(gitbolt::git::GitProcess(dir.toStdString()).run(args));
        };
        QVERIFY(QDir().mkpath(subUp) && QDir().mkpath(super));
        QVERIFY(git(subUp, {"init", "-q"}));
        QVERIFY(git(subUp, {"commit", "-q", "--allow-empty", "-m", "s1"}));
        QVERIFY(git(super, {"init", "-q"}));
        QVERIFY(git(super, {"submodule", "add", "-q", subUp.toStdString(), "sub"}));
        QVERIFY(git(super, {"commit", "-q", "-m", "with sub"}));
        // Not checked out here: as a fresh clone has it.
        QVERIFY(git(super, {"submodule", "deinit", "-q", "-f", "sub"}));
        QVERIFY(!QFileInfo::exists(super + QStringLiteral("/sub/.git")));

        MainWindow window;
        QVERIFY(openIn(window, super));
        auto* manage = action(window, "repository.manage-submodules");
        QVERIFY(manage->isEnabled());
        manage->trigger();
        gitbolt::widgets::SubmoduleWidget* widget = nullptr;
        QTRY_VERIFY((widget = window.findChild<gitbolt::widgets::SubmoduleWidget*>()) != nullptr);
        auto* label = window.findChild<QLabel*>(QStringLiteral("toolbar.opIndicator"));
        QVERIFY(label);

        emit widget->updateRequested(QStringLiteral("sub"));
        QVERIFY(!action(window, "act.commit")->isEnabled());
        QCOMPARE(label->text(), QStringLiteral("Updating submodule sub…"));
        QTRY_VERIFY_WITH_TIMEOUT(action(window, "act.commit")->isEnabled(), 20000);
        QVERIFY(QFileInfo::exists(super + QStringLiteral("/sub/.git")));
        if (auto* dialog = qobject_cast<QDialog*>(widget->window()))
            dialog->close();
    }

    // `git cherry-pick A B` stopped at A on a conflict, and A was then
    // committed (GitBolt's Commit… runs git commit): the sequence is
    // paused, with B to come. git's --abort won't rewind past a commit
    // made during it: it warns "You seem to have moved HEAD. Not
    // rewinding" and exits 0. The question promised "The branch goes
    // back to where it was", and the outcome said "Cherry-pick
    // aborted.", with A still on the branch. Both say so now.
    void abortOfAPausedSequenceSaysTheCommitsStay() {
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit(QStringLiteral("f.txt"), "base\n", QStringLiteral("base")).ok());
        gitbolt::git::GitProcess git(repo.path().toStdString());
        const std::string main = branchOf(git);
        QVERIFY(succeeded(git.run({"checkout", "-q", "-b", "side"})));
        repo.writeFile(QStringLiteral("f.txt"), "side\n");
        QVERIFY(succeeded(git.run({"commit", "-q", "-am", "A"})));
        repo.writeFile(QStringLiteral("g.txt"), "g\n");
        QVERIFY(succeeded(git.run({"add", "g.txt"})));
        QVERIFY(succeeded(git.run({"commit", "-q", "-m", "B"})));
        QVERIFY(succeeded(git.run({"checkout", "-q", main})));
        repo.writeFile(QStringLiteral("f.txt"), "main\n");
        QVERIFY(succeeded(git.run({"commit", "-q", "-am", "M"})));
        QVERIFY(!succeeded(git.run({"cherry-pick", "side~1", "side"})));
        repo.writeFile(QStringLiteral("f.txt"), "resolved\n");
        QVERIFY(succeeded(git.run({"add", "f.txt"})));
        QVERIFY(succeeded(git.run({"commit", "-q", "--no-edit"})));
        const QString picked = output(git.run({"rev-parse", "HEAD"}));

        MainWindow window;
        auto* svc = openIn(window, repo.path());
        QVERIFY(svc);
        auto* bar = window.findChild<gitbolt::widgets::RepoOperationBar*>();
        QVERIFY(bar);
        QTRY_VERIFY_WITH_TIMEOUT(bar->isVisible() && bar->sequencePaused(), 10000);
        QVERIFY(!bar->sequenceSkippable());
        QPushButton* abort = nullptr;
        for (auto* button : bar->findChildren<QPushButton*>())
            if (button->text() == QStringLiteral("Abort"))
                abort = button;
        QVERIFY(abort);
        QVERIFY2(abort->toolTip().contains(QStringLiteral("the commits picked so far stay")),
                 qPrintable(abort->toolTip()));

        BoxAnswerer answer(QMessageBox::Yes);
        abort->click();
        QVERIFY2(answer.text.contains(QStringLiteral("stay on the branch"))
                     && !answer.text.contains(QStringLiteral("goes back")),
                 qPrintable(answer.text));
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().startsWith(
                                     QStringLiteral("Cherry-pick stopped. The commits picked so "
                                                    "far stay on the branch.")),
                                 15000);
        QVERIFY2(window.statusBar()->currentMessage().contains(QStringLiteral("Not rewinding")),
                 qPrintable(window.statusBar()->currentMessage()));
        QCOMPARE(output(git.run({"rev-parse", "HEAD"})), picked);
        QCOMPARE(svc->repository()->state(), gitbolt::git::RepoState::None);
    }

    // A reword whose new message a commit-msg hook turned down stops the
    // rebase with the commit picked under its old message. git's --skip
    // there only goes on: the commit stays. Skip's question said "Its
    // changes are left out of the rebased branch."
    void skipAtARejectedRewordSaysTheCommitStays() {
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit(QStringLiteral("a.txt"), "a\n", QStringLiteral("A")).ok());
        const auto b = repo.writeAndCommit(QStringLiteral("b.txt"), "b\n", QStringLiteral("B original"));
        const auto c = repo.writeAndCommit(QStringLiteral("c.txt"), "c\n", QStringLiteral("C"));
        QVERIFY(b.ok() && c.ok());
        const auto onto = repo.repo().resolveRef("HEAD~2");
        QVERIFY(onto.ok());
        QVERIFY(writeHook(repo.path(), "commit-msg",
                          "if grep -q BAD \"$1\"; then echo 'commit-msg: no BAD' >&2; exit 1; fi\n"));
        gitbolt::git::GitProcess git(repo.path().toStdString());

        using Type = gitbolt::git::RebaseOperationType;
        gitbolt::git::RebasePlan plan;
        plan.onto = *onto;
        plan.head = *c;
        plan.branch = branchOf(git);
        plan.operations = {{Type::Pick, *c, "C"},
                           {Type::Reword, *b, "B original", "BAD\n"}};

        MainWindow window;
        auto* svc = openIn(window, repo.path());
        QVERIFY(svc);
        auto* bar = window.findChild<gitbolt::widgets::RepoOperationBar*>();
        QVERIFY(bar);
        QSignalSpy steps(svc, &GitService::rebaseStepFinished);
        svc->interactiveRebase(plan);
        QTRY_COMPARE_WITH_TIMEOUT(steps.count(), 1, 20000);
        QTRY_VERIFY(bar->isVisible() && bar->state() == gitbolt::git::RepoState::Rebase
                    && !bar->isBusy());
        QCOMPARE(svc->committedRebaseStop(), QStringLiteral("reword"));

        BoxAnswerer answer(QMessageBox::Cancel);
        emit bar->skipRequested();
        QVERIFY2(answer.text.contains(QStringLiteral("stays in the rebased branch under its old "
                                                     "message"))
                     && !answer.text.contains(QStringLiteral("left out")),
                 qPrintable(answer.text));
        QCOMPARE(svc->repository()->state(), gitbolt::git::RepoState::Rebase);
        QVERIFY(succeeded(git.run({"rebase", "--abort"})));
    }

    // A merge (a local op) started while a Fetch ran, and outlived it:
    // the Fetch's outcome went up, cleared after 4 s, and from then on
    // nothing said a merge was still running, every repo action off.
    // Once the outcome has been up a while, the merge says so again.
    // Meanwhile the sidebar's New Branch is off with the rest.
    void localOpSaysAgainWhatItDoesAfterARemoteOp() {
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit(QStringLiteral("a.txt"), "a\n", QStringLiteral("base")).ok());
        QTemporaryDir originDir;
        QVERIFY(originDir.isValid());
        gitbolt::git::GitProcess git(repo.path().toStdString());
        QVERIFY(succeeded(git.run({"init", "-q", "--bare", originDir.path().toStdString()})));
        QVERIFY(succeeded(git.run({"remote", "add", "origin", originDir.path().toStdString()})));
        const std::string main = branchOf(git);
        QVERIFY(succeeded(git.run({"push", "-q", "origin", main})));
        // The Fetch takes 2 s at the server's end.
        QVERIFY(succeeded(git.run({"config", "remote.origin.uploadpack",
                                   "sleep 2; git upload-pack"})));
        QVERIFY(succeeded(git.run({"checkout", "-q", "-b", "topic"})));
        repo.writeFile(QStringLiteral("t.txt"), "t\n");
        QVERIFY(succeeded(git.run({"add", "t.txt"})));
        QVERIFY(succeeded(git.run({"commit", "-q", "-m", "topic"})));
        QVERIFY(succeeded(git.run({"checkout", "-q", main})));
        // The merge's post-merge hook holds it until there's a "go".
        QVERIFY(writeHook(repo.path(), "post-merge",
                          "while [ ! -f .git/go ]; do sleep 0.05; done\n"));

        MainWindow window;
        QVERIFY(openIn(window, repo.path()));
        auto* tree = window.findChild<gitbolt::widgets::BranchTreeWidget*>();
        auto* label = window.findChild<QLabel*>(QStringLiteral("toolbar.opIndicator"));
        auto* newBranch = window.findChild<QAction*>(QStringLiteral("branchTree.newBranch"));
        QVERIFY(tree && label && newBranch);
        const auto release = qScopeGuard([&] { repo.writeFile(QStringLiteral(".git/go"), ""); });

        action(window, "act.fetch")->trigger();
        QTRY_COMPARE(label->text(), QStringLiteral("Fetching from origin…"));
        emit tree->mergeRequested(QStringLiteral("topic"));
        QCOMPARE(window.statusBar()->currentMessage(), QStringLiteral("Merging topic…"));
        QVERIFY(!newBranch->isEnabled());
        QTRY_COMPARE_WITH_TIMEOUT(label->text(), QStringLiteral("✓ Fetch complete."), 15000);
        // Its 4 s, then the merge's word again.
        QTRY_COMPARE_WITH_TIMEOUT(label->text(), QStringLiteral("Merging topic…"), 6000);
        QCOMPARE(window.statusBar()->currentMessage(), QStringLiteral("Merging topic…"));
        QVERIFY(!action(window, "act.commit")->isEnabled());

        repo.writeFile(QStringLiteral(".git/go"), "");
        QTRY_VERIFY_WITH_TIMEOUT(action(window, "act.commit")->isEnabled(), 15000);
        QCOMPARE(label->text(), QString());
        QVERIFY(newBranch->isEnabled());
        QCOMPARE(output(git.run({"log", "-1", "--format=%s"})), QStringLiteral("topic"));
    }

private:
    // Opens `path` the way a second launch's hand-off does, and returns
    // the window's GitService once the repository is on screen.
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

    static QAction* action(MainWindow& window, const char* name) {
        auto* a = window.findChild<QAction*>(QString::fromLatin1(name));
        if (!a)
            qFatal("no action %s", name);
        return a;
    }

    QTemporaryDir settingsDir_;
};

QTEST_MAIN(TestRepoActionLocks)
#include "TestRepoActionLocks.moc"
