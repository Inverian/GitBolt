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
//     asking for a name it then threw away;
//   - a cherry-pick of several commits that git stopped before picking
//     one (a dirty index): Skip is turned down while changes are
//     staged, and Abort says they go too (git's --skip and --abort
//     reset the index);
//   - Skip and Abort look at the stop on a worker (a status, patch-ids),
//     not on the GUI thread, the bar busy meanwhile;
//   - a second launch handing another repository over while a picker
//     or a question is up changes nothing in either repository (Delete
//     branch, Undo, Checkout, Merge, tags, remote Delete, the sidebar's
//     Deletes, New Branch, Rename and Set Upstream, the Reflog and
//     Worktrees windows, Skip, the Commit dialog's Discard), and the
//     modeless windows of the first one close, once a row's context
//     menu up in them is gone (deleted under it, GitBolt crashed);
//   - Continue on a cherry-pick or revert sequence whose stop git never
//     picked, a commit made since, asks before git leaves it out;
//   - Commit & Push pushes though a quiet auto-fetch ran as the commit
//     landed; the in-progress bar shows no merge while a merge's
//     post-merge hook runs; the Commit tab's "contained in" reply of
//     a repository no longer open is dropped; a revealed auto-fetch
//     outlives its repository opened again by a path in another case.
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
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QScopeGuard>
#include <QScopedValueRollback>
#include <QSettings>
#include <QSignalSpy>
#include <QStatusBar>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBrowser>
#include <QTimer>
#include <QTreeView>

#include "../TestProcessHelper.h"
#include "../TestRepoHelper.h"
#include "git/GitProcess.h"
#include "dialogs/CommitDialog.h"
#include "dialogs/ReflogDialog.h"
#include "dialogs/TagDialog.h"
#include "git/Rebase.h"
#include "services/GitService.h"
#include "ui/MainWindow.h"
#include "ui/RepositoryView.h"
#include "widgets/BranchTreeWidget.h"
#include "widgets/RepoOperationBar.h"
#include "widgets/SubmoduleWidget.h"
#include "widgets/WorktreeWidget.h"

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

// A second launch handing `other` over while the window asks something:
// main() opens a path from InstanceServer::pathReceived at once, in
// whatever event loop is turning, a picker's or a question's included.
// At the first modal dialog this opens `other` (openRepositoryAtPath,
// as main() does) and waits for it to land, then answers that dialog
// and every later one: Yes (or OK, or Discard) on a message box; on
// any other dialog, `fill` first (a picker's choice, a form's fields),
// then OK.
class HandOffThenAnswer {
public:
    HandOffThenAnswer(MainWindow& window, GitService* svc, const QString& other,
                      std::function<void(QDialog*)> fill = {}) {
        timer_.setInterval(20);
        QObject::connect(&timer_, &QTimer::timeout, [this, &window, svc, other, fill] {
            if (inside_)
                return;
            auto* modal = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!modal || !modal->isVisible())
                return;
            const QScopedValueRollback<bool> busy(inside_, true);
            if (!handedOff) {
                const std::string before = svc->openKey();
                window.openRepositoryAtPath(other);
                handedOff = QTest::qWaitFor([&] {
                    return svc->isOpen() && !svc->openPending() && svc->openKey() != before;
                }, 15000);
            }
            if (auto* box = qobject_cast<QMessageBox*>(modal)) {
                asked << box->text();
                QAbstractButton* yes = box->button(QMessageBox::Yes);
                if (!yes)
                    yes = box->button(QMessageBox::Ok);
                if (!yes)
                    yes = box->button(QMessageBox::Discard);
                if (yes)
                    yes->click();
                else
                    box->reject();
            } else {
                asked << modal->windowTitle();
                if (fill)
                    fill(modal);
                modal->accept();
            }
        });
        timer_.start();
    }
    bool handedOff = false;
    QStringList asked;

private:
    QTimer timer_;
    bool inside_ = false;
};

// What a hand-off mustn't change: the branches and tags and where they
// point, their upstreams, HEAD, the work tree and its changes (a linked
// worktree's too), and the stashes.
QString fingerprint(const QString& repo) {
    gitbolt::git::GitProcess git(repo.toStdString());
    QStringList parts;
    for (const std::vector<std::string>& args : std::vector<std::vector<std::string>>{
             {"for-each-ref", "--format=%(refname) %(objectname)", "refs/heads", "refs/tags"},
             {"config", "--get-regexp", "^branch\\."},
             {"rev-parse", "HEAD"},
             {"status", "--porcelain"},
             {"diff"},
             {"worktree", "list", "--porcelain"}}) {
        parts << output(git.run(args));
    }
    const QString review = QFileInfo(repo).dir().filePath(
        QFileInfo(repo).fileName() + QStringLiteral("-wt/review/mine.txt"));
    parts << (QFileInfo::exists(review) ? QStringLiteral("review work there")
                                        : QStringLiteral("review work GONE"));
    parts << (QFileInfo::exists(repo + QStringLiteral("/.git/rebase-merge"))
                  ? QStringLiteral("rebase stopped") : QStringLiteral("no rebase"));
    return parts.join(QLatin1Char('\n'));
}

// The row of `name` in a view of the sidebar's branch model, under
// whatever category.
QModelIndex rowOf(const QAbstractItemModel* model, const QString& name,
                  const QModelIndex& parent = {}) {
    for (int row = 0; row < model->rowCount(parent); ++row) {
        const QModelIndex index = model->index(row, 0, parent);
        if (parent.isValid() && index.data().toString() == name)
            return index;
        if (const QModelIndex below = rowOf(model, name, index); below.isValid())
            return below;
    }
    return {};
}

// Right-clicks `view`'s row `index`, as the user would, and picks the
// entry of the context menu whose text starts with `entry` once it is
// up: the menu closed first, as a click on it closes it. Whatever that
// entry opens (a question, a picker) runs in this call.
bool pickFromContextMenu(QAbstractItemView* view, const QModelIndex& index,
                         const QString& entry) {
    view->scrollTo(index);
    const QPoint pos = view->visualRect(index).center();
    if (view->indexAt(pos) != index)
        return false;
    bool picked = false;
    QTimer timer;
    timer.setInterval(20);
    QObject::connect(&timer, &QTimer::timeout, [&] {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        if (!menu)
            return;
        timer.stop();
        QAction* want = nullptr;
        for (QAction* a : menu->actions()) {
            if (a->text().startsWith(entry))
                want = a;
        }
        menu->close();
        if (want && want->isEnabled()) {
            picked = true;
            want->trigger();
        }
    });
    timer.start();
    emit view->customContextMenuRequested(pos);
    return picked;
}

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
        // Asked once a worker has looked at the stop.
        QTRY_VERIFY(!answer.text.isEmpty());
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
        QTRY_VERIFY(!answer.text.isEmpty());
        QVERIFY2(answer.text.contains(QStringLiteral("stays in the rebased branch under its old "
                                                     "message"))
                     && !answer.text.contains(QStringLiteral("left out")),
                 qPrintable(answer.text));
        QCOMPARE(svc->repository()->state(), gitbolt::git::RepoState::Rebase);
        QVERIFY(succeeded(git.run({"rebase", "--abort"})));
    }

    // `git cherry-pick X Y` turned down over staged changes (from a
    // terminal) leaves the sequence with nothing picked: the bar offers
    // Skip and Abort. git's --skip and --abort there reset the index to
    // HEAD (reset --merge), and the staged changes, the user's, went:
    // Skip said "Its changes are left out" of the commit's, and Abort
    // spoke of resolved conflicts. Skip is turned down now, and Abort
    // says what goes. Both look on a worker first (a status, which
    // takes seconds on a big tree), the bar busy meanwhile.
    void skipAndAbortOfASequenceThatPickedNothingKeepStagedWork() {
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit(QStringLiteral("f.txt"), "base\n", QStringLiteral("base")).ok());
        gitbolt::git::GitProcess git(repo.path().toStdString());
        const std::string main = branchOf(git);
        QVERIFY(succeeded(git.run({"checkout", "-q", "-b", "side"})));
        for (const char* name : {"X", "Y"}) {
            repo.writeFile(QString::fromLatin1(name) + QStringLiteral(".txt"), "x\n");
            QVERIFY(succeeded(git.run({"add", std::string(name) + ".txt"})));
            QVERIFY(succeeded(git.run({"commit", "-q", "-m", name})));
        }
        QVERIFY(succeeded(git.run({"checkout", "-q", main})));
        repo.writeFile(QStringLiteral("f.txt"), "staged work\n");
        QVERIFY(succeeded(git.run({"add", "f.txt"})));
        QVERIFY(!succeeded(git.run({"cherry-pick", "side~1", "side"})));

        MainWindow window;
        auto* svc = openIn(window, repo.path());
        QVERIFY(svc);
        auto* bar = window.findChild<gitbolt::widgets::RepoOperationBar*>();
        QVERIFY(bar);
        QTRY_VERIFY_WITH_TIMEOUT(bar->isVisible() && bar->sequencePaused(), 10000);
        QVERIFY(bar->sequenceSkippable());

        {
            BoxAnswerer answer(QMessageBox::Ok);
            emit bar->skipRequested();
            QVERIFY(bar->isBusy());
            QTRY_VERIFY(!answer.text.isEmpty());
            QVERIFY2(answer.text.startsWith(QStringLiteral(
                         "Skipping would discard your staged changes.")),
                     qPrintable(answer.text));
        }
        QVERIFY(!bar->isBusy());
        QCOMPARE(output(git.run({"show", ":f.txt"})), QStringLiteral("staged work"));
        QVERIFY(QFileInfo::exists(repo.path() + QStringLiteral("/.git/sequencer")));
        // And the step's own refusal, for what gets past the window.
        QSignalSpy failed(svc, &GitService::operationFailed);
        svc->sequenceSkip();
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 10000);
        QVERIFY2(failed.at(0).at(1).toString().startsWith(
                     QStringLiteral("Skipping would discard your staged changes")),
                 qPrintable(failed.at(0).at(1).toString()));
        QTRY_VERIFY(!bar->isBusy());
        QCOMPARE(output(git.run({"show", ":f.txt"})), QStringLiteral("staged work"));

        BoxAnswerer answer(QMessageBox::Cancel);
        emit bar->abortRequested();
        QTRY_VERIFY(!answer.text.isEmpty());
        QVERIFY2(answer.text.contains(QStringLiteral("Your staged changes are discarded too.")),
                 qPrintable(answer.text));
        QCOMPARE(output(git.run({"show", ":f.txt"})), QStringLiteral("staged work"));
    }

    // Skip and Abort at a rebase stop asked git what the stop was on
    // the GUI thread: a status, and at a reword's stop picked anew two
    // patch-ids (a diff-tree -p and a patch-id each), seconds on a big
    // commit, the window frozen meanwhile. A worker asks now, the bar
    // busy, and the question comes once it has the answer.
    void skipLooksAtTheStopOffTheGuiThread() {
#ifdef Q_OS_WIN
        QSKIP("the slow git is a shell script");
#else
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit(QStringLiteral("a.txt"), "a\n", QStringLiteral("A")).ok());
        const auto b = repo.writeAndCommit(QStringLiteral("b.txt"), "b\n", QStringLiteral("B"));
        const auto c = repo.writeAndCommit(QStringLiteral("c.txt"), "c\n", QStringLiteral("C"));
        QVERIFY(b.ok() && c.ok());
        gitbolt::git::GitProcess git(repo.path().toStdString());
        const std::string onto = output(git.run({"rev-parse", "HEAD~2"})).toStdString();
        QVERIFY(writeHook(repo.path(), "commit-msg",
                          "if grep -q BAD \"$1\"; then echo 'commit-msg: no BAD' >&2; exit 1; fi\n"));
        // B reworded first, so C is picked anew (not fast-forwarded),
        // and turned down: a stop whose commit only its patch-id tells.
        const auto stopped = git.interactiveRebase(
            onto, "reword " + b->toHex() + " B\nreword " + c->toHex() + " C\n",
            {{b->toHex(), "B, reworded\n"}, {c->toHex(), "BAD C\n"}});
        QVERIFY(stopped.ok() && !stopped->success());
        QCOMPARE(output(git.run({"log", "-1", "--format=%s"})), QStringLiteral("C"));
        QVERIFY(output(git.run({"rev-parse", "HEAD"})).toStdString() != c->toHex());

        // From here, a git whose patch-id takes 1.5 s.
        QTemporaryDir bin;
        QVERIFY(bin.isValid());
        const QString realGit = QStandardPaths::findExecutable(QStringLiteral("git"));
        QVERIFY(!realGit.isEmpty());
        const QString slowGit = bin.filePath(QStringLiteral("git"));
        QVERIFY(writeFile(slowGit, "#!/bin/sh\n"
                                   "case \" $* \" in *\" patch-id \"*) sleep 1.5;; esac\n"
                                   "exec '" + realGit.toUtf8() + "' \"$@\"\n"));
        QVERIFY(QFile::setPermissions(slowGit, QFile::ReadOwner | QFile::WriteOwner
                                                   | QFile::ExeOwner));
        const gitbolt::test::ScopedEnv path("PATH", QFile::encodeName(bin.path()) + ':'
                                                        + qgetenv("PATH"));

        MainWindow window;
        auto* svc = openIn(window, repo.path());
        QVERIFY(svc);
        auto* bar = window.findChild<gitbolt::widgets::RepoOperationBar*>();
        QVERIFY(bar);
        QTRY_VERIFY_WITH_TIMEOUT(bar->isVisible() && !bar->isBusy(), 10000);

        BoxAnswerer answer(QMessageBox::Cancel);
        QElapsedTimer clock;
        clock.start();
        emit bar->skipRequested();
        const qint64 blocked = clock.elapsed();
        QVERIFY2(blocked < 1000, qPrintable(QStringLiteral("the window waited %1 ms").arg(blocked)));
        QVERIFY(bar->isBusy());
        QVERIFY(answer.text.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(!answer.text.isEmpty(), 15000);
        QVERIFY2(answer.text.contains(QStringLiteral("under its old message")),
                 qPrintable(answer.text));
        QVERIFY(!bar->isBusy());
        QCOMPARE(svc->repository()->state(), gitbolt::git::RepoState::Rebase);
#endif
    }

    // A second launch can hand another repository over while a picker
    // or a question of the window's is up (main() opens it at once, in
    // the dialog's event loop). What was asked about one repository was
    // then done to the other: Delete branch deleted its unmerged branch
    // of the same name, Undo last commit undid its commit, a Reset
    // --hard from the Reflog window, or a worktree's Remove, threw its
    // work away. Now nothing is done, and the status bar says so; the
    // modeless windows close once the other repository is open.
    //
    // B is a clone of A (so that A's commits are B's too, as in a
    // clone or a worktree of it), with branches, tags and a linked
    // worktree "review" of the same names, uncommitted work in it.
    void handOffDuringAQuestionChangesNothing_data() {
        QTest::addColumn<QString>("flow");
        QTest::newRow("Delete branch") << QStringLiteral("commands.delete-branch");
        QTest::newRow("Undo last commit") << QStringLiteral("commands.undo-last-commit");
        QTest::newRow("Checkout branch") << QStringLiteral("commands.checkout-branch");
        QTest::newRow("Merge branches") << QStringLiteral("commands.merge-branches");
        QTest::newRow("Create tag") << QStringLiteral("commands.create-tag");
        QTest::newRow("Delete tag") << QStringLiteral("commands.delete-tag");
        QTest::newRow("Delete remote branch") << QStringLiteral("commands.delete-remote-branch");
        QTest::newRow("sidebar Delete") << QStringLiteral("sidebar.delete");
        QTest::newRow("sidebar Delete from remote") << QStringLiteral("sidebar.delete-remote");
        QTest::newRow("sidebar New Branch") << QStringLiteral("branchTree.newBranch");
        QTest::newRow("sidebar Rename") << QStringLiteral("sidebar.rename");
        QTest::newRow("sidebar Set upstream") << QStringLiteral("sidebar.set-upstream");
        QTest::newRow("Commit dialog Discard") << QStringLiteral("commit.discard");
        QTest::newRow("Reflog Reset --hard") << QStringLiteral("view.reflog");
        QTest::newRow("Worktree Remove") << QStringLiteral("repository.manage-worktrees");
        QTest::newRow("Skip at an edit stop") << QStringLiteral("bar.skip");
    }

    void handOffDuringAQuestionChangesNothing() {
        QFETCH(QString, flow);
        QTemporaryDir scratch;
        QVERIFY(scratch.isValid());
        const QString server = scratch.filePath(QStringLiteral("server.git"));
        const QString a = scratch.filePath(QStringLiteral("a"));
        const QString b = scratch.filePath(QStringLiteral("b"));
        const auto git = [](const QString& dir, std::vector<std::string> args) {
            return succeeded(gitbolt::git::GitProcess(dir.toStdString()).run(args));
        };
        QVERIFY(QDir().mkpath(a));
        QVERIFY(git(scratch.path(), {"init", "-q", "--bare", server.toStdString()}));
        QVERIFY(git(a, {"init", "-q"}));
        QVERIFY(git(a, {"commit", "-q", "--allow-empty", "-m", "base"}));
        QVERIFY(git(a, {"commit", "-q", "--allow-empty", "-m", "second"}));
        QVERIFY(git(a, {"branch", "release"}));
        QVERIFY(git(a, {"checkout", "-q", "-b", "feature"}));
        QVERIFY(git(a, {"commit", "-q", "--allow-empty", "-m", "unmerged"}));
        QVERIFY(git(a, {"checkout", "-q", "main"}));
        QVERIFY(git(a, {"tag", "v1"}));
        QVERIFY(git(a, {"remote", "add", "origin", server.toStdString()}));
        QVERIFY(git(a, {"push", "-q", "origin", "main", "release"}));
        QVERIFY(git(scratch.path(), {"clone", "-q", a.toStdString(), b.toStdString()}));
        QVERIFY(git(b, {"branch", "feature", "origin/feature"}));
        for (const QString& repo : {a, b}) {
            const QString review = repo + QStringLiteral("-wt/review");
            QVERIFY(git(repo, {"worktree", "add", "-q", "-b",
                               "review-" + QFileInfo(repo).fileName().toStdString(),
                               review.toStdString()}));
            QVERIFY(writeFile(review + QStringLiteral("/mine.txt"), "uncommitted\n"));
        }
        if (flow == QLatin1String("bar.skip")) {
            // Both stopped at an `edit` of "second", to go on with it.
            for (const QString& repo : {a, b})
                QVERIFY(git(repo, {"-c", "sequence.editor=sed -i.bak 's/^pick/edit/'",
                                   "rebase", "-q", "-i", "HEAD~1"}));
        }
        if (flow == QLatin1String("commit.discard")) {
            // Both with an uncommitted edit of f.txt, theirs alone.
            for (const QString& repo : {a, b}) {
                QVERIFY(writeFile(repo + QStringLiteral("/f.txt"), "one\n"));
                QVERIFY(git(repo, {"add", "f.txt"}));
                QVERIFY(git(repo, {"commit", "-q", "-m", "f"}));
                QVERIFY(writeFile(repo + QStringLiteral("/f.txt"),
                                  "work of " + QFileInfo(repo).fileName().toUtf8() + '\n'));
            }
        }
        const QString beforeA = fingerprint(a);
        const QString beforeB = fingerprint(b);

        MainWindow window;
        auto* svc = openIn(window, a);
        QVERIFY(svc);
        HandOffThenAnswer handOff(window, svc, b, [](QDialog* dialog) {
            if (auto* picker = qobject_cast<QInputDialog*>(dialog)) {
                // Not the branch checked out (main), nor where the
                // server's HEAD points.
                const QStringList items = picker->comboBoxItems();
                for (const QString& want : {QStringLiteral("feature"),
                                            QStringLiteral("origin/release"),
                                            QStringLiteral("v1")}) {
                    if (items.contains(want))
                        picker->setTextValue(want);
                }
                // The sidebar's names: one that is in neither, and an
                // upstream both have.
                if (dialog->windowTitle() == QLatin1String("New Branch"))
                    picker->setTextValue(QStringLiteral("made-while-asked"));
                else if (dialog->windowTitle() == QLatin1String("Rename Branch"))
                    picker->setTextValue(QStringLiteral("renamed-while-asked"));
                else if (dialog->windowTitle() == QLatin1String("Set Upstream"))
                    picker->setTextValue(QStringLiteral("origin/release"));
            } else if (qobject_cast<gitbolt::dialogs::TagDialog*>(dialog)) {
                if (auto* name = dialog->findChild<QLineEdit*>(QStringLiteral("tag.name")))
                    name->setText(QStringLiteral("v2"));
            }
        });
        auto* tree = window.findChild<gitbolt::widgets::BranchTreeWidget*>();
        auto* bar = window.findChild<gitbolt::widgets::RepoOperationBar*>();
        QVERIFY(tree && bar);
        if (flow == QLatin1String("sidebar.delete")) {
            emit tree->deleteBranchRequested(QStringLiteral("feature"));
        } else if (flow == QLatin1String("sidebar.delete-remote")) {
            emit tree->deleteRemoteBranchRequested(QStringLiteral("origin/release"));
        } else if (flow == QLatin1String("view.reflog")) {
            action(window, "view.reflog")->trigger();
            auto* reflog = window.findChild<gitbolt::dialogs::ReflogDialog*>();
            QVERIFY(reflog);
            emit reflog->resetRequested(output(gitbolt::git::GitProcess(a.toStdString())
                                                   .run({"rev-parse", "HEAD~1"})),
                                        QStringLiteral("hard"));
        } else if (flow == QLatin1String("repository.manage-worktrees")) {
            action(window, "repository.manage-worktrees")->trigger();
            auto* worktrees = window.findChild<gitbolt::widgets::WorktreeWidget*>();
            QVERIFY(worktrees);
            emit worktrees->removeRequested(QStringLiteral("review"));
        } else if (flow == QLatin1String("bar.skip")) {
            QTRY_VERIFY_WITH_TIMEOUT(bar->isVisible() && !bar->isBusy(), 10000);
            emit bar->skipRequested();
            QTRY_VERIFY_WITH_TIMEOUT(handOff.handedOff && !handOff.asked.isEmpty(), 15000);
        } else if (flow == QLatin1String("sidebar.rename")
                   || flow == QLatin1String("sidebar.set-upstream")) {
            // From the branch's context menu, as the user does.
            auto* view = tree->findChild<QTreeView*>(QStringLiteral("branchTree.view"));
            QVERIFY(view);
            QModelIndex feature;
            QTRY_VERIFY((feature = rowOf(view->model(), QStringLiteral("feature"))).isValid());
            QVERIFY(pickFromContextMenu(view, feature,
                                        flow == QLatin1String("sidebar.rename")
                                            ? QStringLiteral("Rename '")
                                            : QStringLiteral("Set Upstream")));
        } else if (flow == QLatin1String("commit.discard")) {
            action(window, "act.commit")->trigger();
            auto* dialog = window.findChild<gitbolt::dialogs::CommitDialog*>();
            QVERIFY(dialog);
            auto* unstaged = dialog->findChild<QAbstractItemView*>(
                QStringLiteral("commit.unstagedList"));
            auto* discard = dialog->findChild<QPushButton*>(QStringLiteral("commit.discardBtn"));
            QVERIFY(unstaged && discard);
            QTRY_VERIFY_WITH_TIMEOUT(unstaged->model()->rowCount() == 1, 10000);
            unstaged->setCurrentIndex(unstaged->model()->index(0, 0));
            unstaged->selectionModel()->select(unstaged->model()->index(0, 0),
                                               QItemSelectionModel::ClearAndSelect);
            QTRY_VERIFY(discard->isEnabled());
            discard->click();
        } else {
            action(window, flow.toLatin1().constData())->trigger();
        }
        QVERIFY2(handOff.handedOff, qPrintable(handOff.asked.join(QStringLiteral(" | "))));
        QVERIFY(!handOff.asked.isEmpty());
        QVERIFY2(window.statusBar()->currentMessage().startsWith(
                     QStringLiteral("Another repository was opened; ")),
                 qPrintable(window.statusBar()->currentMessage()));
        // Whatever a local op, a remote op or a step would have run, it
        // has had time to.
        QTest::qWait(1000);
        QTRY_VERIFY_WITH_TIMEOUT(action(window, "act.commit")->isEnabled(), 15000);
        QCOMPARE(fingerprint(b), beforeB);
        QCOMPARE(fingerprint(a), beforeA);
        QVERIFY2(window.windowTitle().startsWith(QStringLiteral("b  -  ")),
                 qPrintable(window.windowTitle()));
        // The modeless windows of A are gone, B open.
        QTRY_VERIFY(!window.findChild<gitbolt::dialogs::ReflogDialog*>());
        QTRY_VERIFY(!window.findChild<gitbolt::widgets::WorktreeWidget*>());
    }

    // The sidebar's New Branch, Rename and Set Upstream ask in the
    // window now (they asked in the sidebar, and went to whichever
    // repository was open once answered): with no hand-off, what is
    // answered is done, to the repository open.
    void sidebarBranchQuestionsActOnTheOpenRepository() {
        QTemporaryDir scratch;
        QVERIFY(scratch.isValid());
        const QString server = scratch.filePath(QStringLiteral("server.git"));
        const QString work = scratch.filePath(QStringLiteral("work"));
        gitbolt::git::GitProcess git(work.toStdString());
        QVERIFY(QDir().mkpath(work));
        QVERIFY(succeeded(gitbolt::git::GitProcess(scratch.path().toStdString())
                              .run({"init", "-q", "--bare", server.toStdString()})));
        QVERIFY(succeeded(git.run({"init", "-q"})));
        QVERIFY(succeeded(git.run({"commit", "-q", "--allow-empty", "-m", "base"})));
        QVERIFY(succeeded(git.run({"branch", "feature"})));
        QVERIFY(succeeded(git.run({"remote", "add", "origin", server.toStdString()})));
        QVERIFY(succeeded(git.run({"push", "-q", "origin", "main", "feature"})));

        MainWindow window;
        auto* svc = openIn(window, work);
        QVERIFY(svc);
        auto* tree = window.findChild<gitbolt::widgets::BranchTreeWidget*>();
        QVERIFY(tree);
        auto* view = tree->findChild<QTreeView*>(QStringLiteral("branchTree.view"));
        QVERIFY(view);
        // Answers the next text question with `text`, noting its title.
        QString asked;
        const auto answerWith = [&asked](const QString& text) {
            auto* timer = new QTimer;
            timer->setInterval(20);
            QObject::connect(timer, &QTimer::timeout, [timer, text, &asked] {
                auto* input = qobject_cast<QInputDialog*>(QApplication::activeModalWidget());
                if (!input)
                    return;
                timer->stop();
                timer->deleteLater();
                asked = input->windowTitle();
                input->setTextValue(text);
                input->accept();
            });
            timer->start();
        };
        const auto branches = [&] {
            return output(git.run({"for-each-ref", "--format=%(refname:short)", "refs/heads"}));
        };

        answerWith(QStringLiteral("made-here"));
        action(window, "branchTree.newBranch")->trigger();
        QCOMPARE(asked, QStringLiteral("New Branch"));
        QTRY_COMPARE_WITH_TIMEOUT(branches(), QStringLiteral("feature\nmade-here\nmain"), 10000);

        QModelIndex feature;
        QTRY_VERIFY((feature = rowOf(view->model(), QStringLiteral("feature"))).isValid());
        answerWith(QStringLiteral("origin/main"));
        QVERIFY(pickFromContextMenu(view, feature, QStringLiteral("Set Upstream")));
        QCOMPARE(asked, QStringLiteral("Set Upstream"));
        QTRY_COMPARE_WITH_TIMEOUT(output(git.run({"rev-parse", "--abbrev-ref", "feature@{u}"})),
                                  QStringLiteral("origin/main"), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(action(window, "act.commit")->isEnabled(), 10000);

        QTRY_VERIFY((feature = rowOf(view->model(), QStringLiteral("feature"))).isValid());
        answerWith(QStringLiteral("renamed"));
        QVERIFY(pickFromContextMenu(view, feature, QStringLiteral("Rename '")));
        QCOMPARE(asked, QStringLiteral("Rename Branch"));
        QTRY_COMPARE_WITH_TIMEOUT(branches(), QStringLiteral("made-here\nmain\nrenamed"), 10000);
    }

    // A row's context menu in the Reflog or Worktrees window is a QMenu
    // on the stack of the window's slot, parented to the window, in
    // exec(): up, but no modal widget. An open that landed meanwhile (a
    // second launch's hand-off, a slow open from Recent) closed the
    // window there and then, and WA_DeleteOnClose deleted it, the menu
    // with it: GitBolt crashed once exec() returned. The window waits
    // for the menu now, and closes once it is gone.
    void contextMenuOutlivesAHandOff_data() {
        QTest::addColumn<QString>("flow");
        QTest::newRow("Reflog") << QStringLiteral("view.reflog");
        QTest::newRow("Worktrees") << QStringLiteral("repository.manage-worktrees");
    }

    void contextMenuOutlivesAHandOff() {
        QFETCH(QString, flow);
        gitbolt::test::TestRepo a;
        gitbolt::test::TestRepo b;
        QVERIFY(a.writeAndCommit(QStringLiteral("a.txt"), "a\n", QStringLiteral("base")).ok());
        QVERIFY(a.writeAndCommit(QStringLiteral("a.txt"), "b\n", QStringLiteral("second")).ok());
        QVERIFY(b.writeAndCommit(QStringLiteral("b.txt"), "b\n", QStringLiteral("base")).ok());
        QTemporaryDir linked;
        QVERIFY(linked.isValid());
        gitbolt::git::GitProcess git(a.path().toStdString());
        QVERIFY(succeeded(git.run({"worktree", "add", "-q", "-b", "review",
                                   linked.filePath(QStringLiteral("review")).toStdString()})));

        MainWindow window;
        auto* svc = openIn(window, a.path());
        QVERIFY(svc);
        action(window, flow.toLatin1().constData())->trigger();
        QPointer<QWidget> opened;
        QAbstractItemView* rows = nullptr;
        if (flow == QLatin1String("view.reflog")) {
            auto* reflog = window.findChild<gitbolt::dialogs::ReflogDialog*>();
            QVERIFY(reflog);
            opened = reflog;
            rows = reflog->findChild<QAbstractItemView*>();
        } else {
            auto* worktrees = window.findChild<gitbolt::widgets::WorktreeWidget*>();
            QVERIFY(worktrees);
            opened = worktrees->window();
            rows = worktrees->findChild<QAbstractItemView*>();
        }
        QVERIFY(opened && opened != &window && rows);
        QTRY_VERIFY_WITH_TIMEOUT(rows->model()->rowCount() > 0, 10000);
        const QModelIndex row = rows->model()->index(rows->model()->rowCount() - 1, 0);
        rows->scrollTo(row);

        // While the menu is up: B opens, as from a second launch, and
        // what that sets off (the window's close, deferred deletes) has
        // its time.
        bool menuUp = false;
        bool handedOff = false;
        bool aliveUnderTheMenu = false;
        QTimer timer;
        timer.setInterval(20);
        QObject::connect(&timer, &QTimer::timeout, [&] {
            auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            if (!menu)
                return;
            timer.stop();
            menuUp = true;
            const std::string before = svc->openKey();
            window.openRepositoryAtPath(b.path());
            handedOff = QTest::qWaitFor([&] {
                return svc->isOpen() && !svc->openPending() && svc->openKey() != before;
            }, 15000);
            QTest::qWait(500);
            aliveUnderTheMenu = !opened.isNull();
            menu->close();
        });
        timer.start();
        emit rows->customContextMenuRequested(rows->visualRect(row).center());
        QVERIFY(menuUp);
        QVERIFY(handedOff);
        QVERIFY(aliveUnderTheMenu);
        // Then it closes, A's window, with B open.
        QTRY_VERIFY(opened.isNull());
    }

    // `git cherry-pick X Y` (or a revert) turned down over staged
    // changes, from a terminal, and the changes then committed, as
    // git's hint says to: HEAD has moved, so the bar offers Continue
    // again, and git's --continue takes X for done though it was never
    // picked. Continue left it out with "Cherry-pick complete.". It asks
    // first now, and goes on without X only when told to.
    void continueOfASequenceThatNeverPickedItsStopAsksFirst_data() {
        QTest::addColumn<bool>("revert");
        QTest::newRow("cherry-pick") << false;
        QTest::newRow("revert") << true;
    }

    void continueOfASequenceThatNeverPickedItsStopAsksFirst() {
        QFETCH(bool, revert);
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit(QStringLiteral("f.txt"), "base\n", QStringLiteral("base")).ok());
        gitbolt::git::GitProcess git(repo.path().toStdString());
        const std::string main = branchOf(git);
        if (!revert)
            QVERIFY(succeeded(git.run({"checkout", "-q", "-b", "side"})));
        // Authored a while back, as a commit to pick is: one of the
        // user's in the same second would pass for its pick.
        for (const char* name : {"X", "Y"}) {
            repo.writeFile(QString::fromLatin1(name) + QStringLiteral(".txt"), "x\n");
            QVERIFY(succeeded(git.run({"add", std::string(name) + ".txt"})));
            QVERIFY(succeeded(git.run({"commit", "-q", "--date=2020-01-02T03:04:05", "-m",
                                       name})));
        }
        if (!revert)
            QVERIFY(succeeded(git.run({"checkout", "-q", main})));
        repo.writeFile(QStringLiteral("f.txt"), "staged work\n");
        QVERIFY(succeeded(git.run({"add", "f.txt"})));
        QVERIFY(!succeeded(revert ? git.run({"revert", "--no-edit", "HEAD", "HEAD~1"})
                                  : git.run({"cherry-pick", "side~1", "side"})));
        QVERIFY(succeeded(git.run({"commit", "-q", "-m", "my staged work"})));
        const QString before = output(git.run({"log", "--format=%s"}));

        MainWindow window;
        auto* svc = openIn(window, repo.path());
        QVERIFY(svc);
        auto* bar = window.findChild<gitbolt::widgets::RepoOperationBar*>();
        QVERIFY(bar);
        QTRY_VERIFY_WITH_TIMEOUT(bar->isVisible() && bar->sequencePaused() && !bar->isBusy(),
                                 10000);
        QVERIFY(!bar->sequenceSkippable());

        {
            BoxAnswerer answer(QMessageBox::Cancel);
            emit bar->continueRequested();
            QVERIFY(bar->isBusy());
            QTRY_VERIFY_WITH_TIMEOUT(!answer.text.isEmpty(), 15000);
            QVERIFY2(answer.text.startsWith(revert ? QStringLiteral("Continue without reverting ")
                                                   : QStringLiteral("Continue without "))
                         && answer.text.contains(revert ? QStringLiteral(" Y?")
                                                        : QStringLiteral(" X?")),
                     qPrintable(answer.text));
        }
        QTRY_VERIFY(!bar->isBusy());
        QVERIFY(QFileInfo::exists(repo.path() + QStringLiteral("/.git/sequencer")));
        QCOMPARE(output(git.run({"log", "--format=%s"})), before);

        // Told to: git goes on with the rest, the first left out.
        BoxAnswerer answer(QMessageBox::Yes);
        emit bar->continueRequested();
        QTRY_VERIFY_WITH_TIMEOUT(!answer.text.isEmpty(), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(repo.path() + QStringLiteral("/.git/sequencer")),
                                 15000);
        QCOMPARE(output(git.run({"log", "-1", "--format=%s"})),
                 revert ? QStringLiteral("Revert \"X\"") : QStringLiteral("Y"));
        QTRY_VERIFY(!bar->isBusy());
    }

    // Commit & Push, the commit landing while a quiet auto-fetch (a
    // periodic tick) runs: Push was left enabled, so the push was asked
    // for and then turned away with the generic "Another remote
    // operation is still running…", the commit never pushed. The tick
    // gives way now: it is stopped, and the push held for the moment
    // its git takes to wind down.
    void commitAndPushDuringAQuietAutoFetchPushes() {
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit(QStringLiteral("a.txt"), "a\n", QStringLiteral("base")).ok());
        QTemporaryDir originDir;
        QVERIFY(originDir.isValid());
        gitbolt::git::GitProcess git(repo.path().toStdString());
        gitbolt::git::GitProcess origin(originDir.path().toStdString());
        QVERIFY(succeeded(git.run({"init", "-q", "--bare", originDir.path().toStdString()})));
        QVERIFY(succeeded(git.run({"remote", "add", "origin", originDir.path().toStdString()})));
        const std::string main = branchOf(git);
        QVERIFY(succeeded(git.run({"push", "-q", "-u", "origin", main})));
        // A fetch that takes its time at the server's end, and says
        // when it has started.
        const QString started = repo.path() + QStringLiteral("/.git/fetch-started");
        QVERIFY(succeeded(git.run({"config", "remote.origin.uploadpack",
                                   "touch '" + started.toStdString()
                                       + "'; sleep 5; git upload-pack"})));

        MainWindow window;
        auto* svc = openIn(window, repo.path());
        QVERIFY(svc);
        auto* periodic = action(window, "plugins.periodic-background-fetch");
        if (!periodic->isChecked()) {
            // It asks for the interval first: the default.
            QTimer accept;
            accept.setInterval(20);
            QObject::connect(&accept, &QTimer::timeout, [&accept] {
                if (auto* ask = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {
                    accept.stop();
                    ask->accept();
                }
            });
            accept.start();
            periodic->trigger();
        }
        // Saved: off again for the tests after this one.
        const auto switchOff = qScopeGuard([periodic] {
            if (periodic->isChecked())
                periodic->trigger();
        });
        auto* timer = window.findChild<QTimer*>(QStringLiteral("periodicFetchTimer"));
        QVERIFY(timer && timer->isActive());
        QMetaObject::invokeMethod(timer, "timeout", Qt::QueuedConnection);
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(started), 10000);
        QVERIFY(action(window, "act.push")->isEnabled());

        // The commit, then the dialog's word that it has landed.
        QVERIFY(succeeded(git.run({"commit", "-q", "--allow-empty", "-m", "to push"})));
        const QString tip = output(git.run({"rev-parse", "HEAD"}));
        action(window, "act.commit")->trigger();
        auto* dialog = window.findChild<gitbolt::dialogs::CommitDialog*>();
        QVERIFY(dialog);
        emit dialog->pushAfterCommitRequested();
        QTRY_COMPARE_WITH_TIMEOUT(output(origin.run({"rev-parse", main})), tip, 15000);
        QTRY_COMPARE_WITH_TIMEOUT(window.statusBar()->currentMessage(),
                                  QStringLiteral("Push complete."), 10000);
        dialog->close();
    }

    // A merge's post-merge hook runs before git removes MERGE_HEAD. A
    // status refresh meanwhile (the watcher's) put the in-progress bar
    // up: "A merge is in progress. Commit to finish it.", over a log
    // that already had the merge commit. The bar takes no state while a
    // local op runs; the op's end brings the real one.
    void barShowsNoMergeWhileTheMergesHookRuns() {
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit(QStringLiteral("a.txt"), "a\n", QStringLiteral("base")).ok());
        gitbolt::git::GitProcess git(repo.path().toStdString());
        const std::string main = branchOf(git);
        QVERIFY(succeeded(git.run({"checkout", "-q", "-b", "topic"})));
        QVERIFY(succeeded(git.run({"commit", "-q", "--allow-empty", "-m", "topic"})));
        QVERIFY(succeeded(git.run({"checkout", "-q", main})));
        QVERIFY(succeeded(git.run({"commit", "-q", "--allow-empty", "-m", "main"})));
        QVERIFY(writeHook(repo.path(), "post-merge",
                          "touch .git/hook-started\n"
                          "while [ ! -f .git/go ]; do sleep 0.05; done\n"));

        MainWindow window;
        auto* svc = openIn(window, repo.path());
        QVERIFY(svc);
        auto* tree = window.findChild<gitbolt::widgets::BranchTreeWidget*>();
        auto* bar = window.findChild<gitbolt::widgets::RepoOperationBar*>();
        QVERIFY(tree && bar);
        const auto release = qScopeGuard([&] { repo.writeFile(QStringLiteral(".git/go"), ""); });

        emit tree->mergeRequested(QStringLiteral("topic"));
        QTRY_VERIFY_WITH_TIMEOUT(
            QFileInfo::exists(repo.path() + QStringLiteral("/.git/hook-started")), 10000);
        QVERIFY(QFileInfo::exists(repo.path() + QStringLiteral("/.git/MERGE_HEAD")));
        QSignalSpy refreshed(svc, &GitService::repoStateReady);
        svc->refreshStatus();
        QTRY_VERIFY_WITH_TIMEOUT(!refreshed.isEmpty(), 10000);
        QCOMPARE(refreshed.last().at(0).value<gitbolt::git::RepoState>(),
                 gitbolt::git::RepoState::Merge);
        QCOMPARE(bar->state(), gitbolt::git::RepoState::None);
        QVERIFY(bar->isHidden());

        repo.writeFile(QStringLiteral(".git/go"), "");
        QTRY_VERIFY_WITH_TIMEOUT(action(window, "act.commit")->isEnabled(), 15000);
        QTRY_VERIFY(refreshed.last().at(0).value<gitbolt::git::RepoState>()
                    == gitbolt::git::RepoState::None);
        QCOMPARE(bar->state(), gitbolt::git::RepoState::None);
        QVERIFY(bar->isHidden());
        QCOMPARE(output(git.run({"log", "-1", "--format=%p"})).count(QLatin1Char(' ')), 1);
    }

    // The Commit tab asks git which branches contain the commit shown,
    // on a worker. Its reply was taken whenever no other commit had
    // been shown since: a repository opened meanwhile (which shows
    // none) got the previous one's commit, message and all, under its
    // title. Opening, or showing no commit, drops it now.
    void containedInReplyStaysWithItsRepository() {
#ifdef Q_OS_WIN
        QSKIP("the slow git is a shell script");
#else
        // A git whose `branch --contains` takes 2 s.
        QTemporaryDir bin;
        QVERIFY(bin.isValid());
        const QString realGit = QStandardPaths::findExecutable(QStringLiteral("git"));
        QVERIFY(!realGit.isEmpty());
        const QString slowGit = bin.filePath(QStringLiteral("git"));
        QVERIFY(writeFile(slowGit, "#!/bin/sh\n"
                                   "case \" $* \" in *\" --contains \"*) sleep 2;; esac\n"
                                   "exec '" + realGit.toUtf8() + "' \"$@\"\n"));
        QVERIFY(QFile::setPermissions(slowGit, QFile::ReadOwner | QFile::WriteOwner
                                                   | QFile::ExeOwner));
        const gitbolt::test::ScopedEnv path("PATH", QFile::encodeName(bin.path()) + ':'
                                                        + qgetenv("PATH"));

        gitbolt::test::TestRepo a;
        gitbolt::test::TestRepo b;
        QVERIFY(a.writeAndCommit(QStringLiteral("a.txt"), "a\n",
                                 QStringLiteral("ALPHA-ONLY-SUBJECT")).ok());
        QVERIFY(b.writeAndCommit(QStringLiteral("b.txt"), "b\n", QStringLiteral("beta")).ok());
        const QString alpha = output(gitbolt::git::GitProcess(a.path().toStdString())
                                         .run({"rev-parse", "HEAD"}));

        MainWindow window;
        auto* svc = openIn(window, a.path());
        QVERIFY(svc);
        auto* view = window.findChild<gitbolt::ui::RepositoryView*>();
        QVERIFY(view);
        const auto shown = [view] {
            QString text;
            for (auto* browser : view->findChildren<QTextBrowser*>())
                text += browser->toPlainText();
            return text;
        };
        // Once the log has it.
        const auto select = [view](const QString& hash) {
            return QMetaObject::invokeMethod(view, "onCommitSelected", Q_ARG(QString, hash));
        };
        QTRY_VERIFY_WITH_TIMEOUT(select(alpha) && shown().contains(QStringLiteral("ALPHA-ONLY-SUBJECT")),
                                 10000);

        // B opens while A's query is out.
        QVERIFY(openIn(window, b.path()) == svc);
        QVERIFY2(!shown().contains(QStringLiteral("ALPHA-ONLY-SUBJECT")), qPrintable(shown()));
        QTest::qWait(3000);
        QVERIFY2(!shown().contains(QStringLiteral("ALPHA-ONLY-SUBJECT")), qPrintable(shown()));

        // Showing no commit drops the query too.
        const QString beta = output(gitbolt::git::GitProcess(b.path().toStdString())
                                        .run({"rev-parse", "HEAD"}));
        QTRY_VERIFY_WITH_TIMEOUT(select(beta) && shown().contains(QStringLiteral("beta")), 10000);
        QVERIFY(select(QString()));
        QTest::qWait(3000);
        QVERIFY2(!shown().contains(QStringLiteral("beta")), qPrintable(shown()));
#endif
    }

    // A Fetch click shows the auto-fetch running (reveals it). The same
    // repository opened again, by a path typed in another case (a
    // second launch's argument, where paths ignore case), was taken for
    // another repository: the fetch was cancelled ("Fetch cancelled.").
    // libgit2 keeps the case it was given on Windows; the window now
    // compares the service's case-folded key. (macOS resolves the case
    // itself, so this passed there before too; Linux paths don't
    // ignore case.)
    void revealedAutoFetchOutlivesTheSameRepositoryInAnotherCase() {
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit(QStringLiteral("a.txt"), "a\n", QStringLiteral("base")).ok());
        QString flipped = repo.path();
        for (qsizetype i = flipped.lastIndexOf(QLatin1Char('/')) + 1; i < flipped.size(); ++i)
            flipped[i] = flipped[i].isUpper() ? flipped[i].toLower() : flipped[i].toUpper();
        if (flipped == repo.path() || !QFileInfo::exists(flipped + QStringLiteral("/a.txt")))
            QSKIP("paths here don't ignore case");
        QTemporaryDir originDir;
        QVERIFY(originDir.isValid());
        gitbolt::git::GitProcess git(repo.path().toStdString());
        QVERIFY(succeeded(git.run({"init", "-q", "--bare", originDir.path().toStdString()})));
        QVERIFY(succeeded(git.run({"remote", "add", "origin", originDir.path().toStdString()})));
        QVERIFY(succeeded(git.run({"push", "-q", "origin", branchOf(git)})));
        const QString started = repo.path() + QStringLiteral("/.git/fetch-started");
        QVERIFY(succeeded(git.run({"config", "remote.origin.uploadpack",
                                   "touch '" + started.toStdString()
                                       + "'; sleep 4; git upload-pack"})));

        MainWindow window;
        auto* svc = openIn(window, repo.path());
        QVERIFY(svc);
        auto* label = window.findChild<QLabel*>(QStringLiteral("toolbar.opIndicator"));
        QVERIFY(label);
        auto* periodic = action(window, "plugins.periodic-background-fetch");
        if (!periodic->isChecked()) {
            QTimer accept;
            accept.setInterval(20);
            QObject::connect(&accept, &QTimer::timeout, [&accept] {
                if (auto* ask = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {
                    accept.stop();
                    ask->accept();
                }
            });
            accept.start();
            periodic->trigger();
        }
        const auto switchOff = qScopeGuard([periodic] {
            if (periodic->isChecked())
                periodic->trigger();
        });
        auto* timer = window.findChild<QTimer*>(QStringLiteral("periodicFetchTimer"));
        QVERIFY(timer && timer->isActive());
        QMetaObject::invokeMethod(timer, "timeout", Qt::QueuedConnection);
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(started), 10000);
        action(window, "act.fetch")->trigger();
        QCOMPARE(label->text(), QStringLiteral("Fetching from origin…"));

        QVERIFY(openIn(window, flipped) == svc);
        QVERIFY2(label->text() != QStringLiteral("Fetch cancelled."), qPrintable(label->text()));
        QTRY_COMPARE_WITH_TIMEOUT(label->text(), QStringLiteral("✓ Fetch complete."), 15000);
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
