#pragma once

#include <QObject>
#include <QString>

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

    RepoCommandController(services::GitService* svc,
                          QWidget* dialogParent,
                          QObject* parent = nullptr);

    /// Returns true when a repository is open AND the command exited
    /// zero. On failure shows a QMessageBox::warning titled
    /// `failTitle` carrying git's stderr (falling back to stdout,
    /// then the exit code), parented to `parentOverride` when given.
    /// The refresh set runs on success AND failure — a failed
    /// command (e.g. a conflicted merge) can still have changed the
    /// repository, and the UI must reflect that.
    bool run(const std::vector<std::string>& args,
             const QString& failTitle,
             RefreshSet refresh = RefreshNone,
             QWidget* parentOverride = nullptr,
             int timeoutMs = 30000);

private:
    services::GitService* svc_;
    QWidget* dialogParent_;
};

Q_DECLARE_OPERATORS_FOR_FLAGS(RepoCommandController::RefreshSet)

} // namespace gitbolt::ui
