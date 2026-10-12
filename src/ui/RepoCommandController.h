#pragma once

#include "git/GitProcess.h"

#include <QFuture>
#include <QObject>
#include <QString>

#include <functional>
#include <string>
#include <vector>

class QWidget;

namespace gitbolt::services { class GitService; }

namespace gitbolt::ui {

/// The shared spine of every "menu action shells out to git"
/// handler: open-repo guard → run the command via
/// GitService::process() → warning dialog with git's stderr on
/// failure → kick the requested refresh set. This exact
/// guard/run/report/refresh sequence was pasted (with small
/// mutations) through the menu builders ~20 times.
///
/// git runs on a pool thread, one local op at a time: on the GUI
/// thread, a merge whose post-merge hook took a while froze the window
/// until git's timeout, which then killed git and reported the merge,
/// done by then, as failed. busyChanged lets the window show the op and
/// keep the repository's actions off while it runs.
class RepoCommandController : public QObject {
    Q_OBJECT
public:
    enum RefreshFlag {
        RefreshNone       = 0,
        RefreshStatus     = 1 << 0,
        RefreshLog        = 1 << 1,
        RefreshBranches   = 1 << 2,
        RefreshStashes    = 1 << 3,
        RefreshSubmodules = 1 << 4,
        RefreshTags       = 1 << 5,
    };
    Q_DECLARE_FLAGS(RefreshSet, RefreshFlag)

    /// How long a local op's git may take: five minutes, as for git's
    /// other long local work (gc, fsck, a rebase step). Nothing waits
    /// on it any more, so a slow hook or filter needn't be cut off.
    static constexpr int kTimeoutMs = 300000;

    RepoCommandController(services::GitService* svc,
                          QWidget* dialogParent,
                          QObject* parent = nullptr);

    /// One local op. `work` runs on a pool thread with a GitProcess for
    /// the open repository (stopped by GitService::cancelGitOps()) and
    /// must touch nothing else; `finished` runs on the GUI thread once
    /// it's done, unless another repository was opened meanwhile (the
    /// outcome is that one's no more). The refresh set runs either way,
    /// after `finished`. False, starting nothing, when no repository is
    /// open or another local op still runs (then with refused()).
    bool runJob(const QString& busyMessage,
                std::function<void(const git::GitProcess&)> work,
                std::function<void()> finished,
                RefreshSet refresh = RefreshNone);

    /// runJob() for one git command. On failure a QMessageBox::warning
    /// titled `failTitle` shows git's stderr (falling back to stdout,
    /// then the exit code), parented to `parentOverride` while it's
    /// still there. `done` then gets whether git exited zero. The
    /// refresh set runs on success AND failure — a failed command (e.g.
    /// a conflicted merge) can still have changed the repository, and
    /// the UI must reflect that.
    bool runAsync(const std::vector<std::string>& args,
                  const QString& failTitle,
                  RefreshSet refresh,
                  const QString& busyMessage,
                  std::function<void(bool)> done = {},
                  QWidget* parentOverride = nullptr,
                  int timeoutMs = kTimeoutMs);

    /// Whether a local op is running.
    bool busy() const { return busy_; }

    /// Waits for the running op's job, if any; for shutdown, once
    /// GitService::cancelGitOps() has stopped its git. Its `finished`
    /// doesn't run.
    void waitForFinished();

signals:
    /// A local op started, saying what it does, or ended (`message`
    /// empty).
    void busyChanged(bool busy, const QString& message);
    /// runJob() turned an op down: another one still runs.
    void refused();

private:
    void refreshAfter(RefreshSet refresh);

    services::GitService* svc_;
    QWidget* dialogParent_;
    bool busy_ = false;
    QFuture<void> job_;
};

Q_DECLARE_OPERATORS_FOR_FLAGS(RepoCommandController::RefreshSet)

} // namespace gitbolt::ui
