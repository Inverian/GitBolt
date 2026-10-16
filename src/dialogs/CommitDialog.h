#pragma once

#include "git/Status.h"

#include <QDialog>
#include <QString>
#include <QStringList>
#include <string>
#include <vector>

class QAction;
class QCheckBox;
class QCloseEvent;
class QLabel;
class QLineEdit;
class QListView;
class QPushButton;
class QShortcut;
class QShowEvent;
class QSortFilterProxyModel;
class QSplitter;
class QToolBar;

namespace gitbolt::services { class GitService; }
namespace gitbolt::conf     { class SettingsService; }
namespace gitbolt::models   { class FileStatusModel; }
namespace gitbolt::widgets {
class CommitMessageEdit;
class DiffViewerWidget;
} // namespace gitbolt::widgets

namespace gitbolt::dialogs {

/// Modeless commit window modeled on Git Extensions' "Commit" dialog.
///
/// Layout:
///
///   ┌──────────────────────────────────┬──────────────────────────────┐
///   │ Unstaged Changes (N)             │                              │
///   │   [filter]                       │                              │
///   │   [file list]                    │   Diff viewer                │
///   │   [Stage] [Stage all] [Discard]  │                              │
///   ├──────────────────────────────────┼──────────────────────────────┤
///   │ Staged Changes (N)               │   ┌────────┐ ┌──────────────┐│
///   │   [filter]                       │   │Commit  │ │ Commit msg   ││
///   │   [file list]                    │   │Cmt&Push│ │ editor       ││
///   │   [Unstage] [Unstage all]        │   │☐ Amend │ │              ││
///   └──────────────────────────────────┴──────────────────────────────┘
///   │ Committer …                  master      Staged 0/4            │
///   └─────────────────────────────────────────────────────────────────┘
///
/// Title: "<repoName> - Commit to <branch>"
class CommitDialog : public QDialog {
    Q_OBJECT
public:
    CommitDialog(services::GitService* svc,
                 conf::SettingsService* settings,
                 QWidget* parent = nullptr);
    ~CommitDialog() override;

    /// Kick a fresh status refresh so the panes reflect the current
    /// working-tree state. Called on show().
    void refresh();

    /// While a local operation (a merge, a reset, `git am`) runs on the
    /// repository: no commit, and no staging, starts on top of it.
    /// MainWindow follows its busy state with this.
    void setLocalOpBusy(bool busy);

signals:
    /// Emitted after a "Commit & Push" commit lands successfully.
    /// MainWindow triggers its toolbar Push action in response, so
    /// the push half gets the same inline feedback (wait cursor,
    /// toolbar label, status bar) as a manual push.
    void pushAfterCommitRequested();

    /// A question of the dialog's (Discard's) was answered with another
    /// repository open, or opening: `notDone` says what wasn't done.
    /// MainWindow says so on its status bar.
    void repositoryChangedUnderQuestion(const QString& notDone);

protected:
    void showEvent(QShowEvent* e) override;
    void closeEvent(QCloseEvent* e) override;

private slots:
    void onStatusReady(std::vector<gitbolt::git::StatusEntry> entries);
    void onUnstagedSelectionChanged();
    void onStagedSelectionChanged();
    void onUnstagedActivated(const QModelIndex& index);
    void onStagedActivated(const QModelIndex& index);
    void onCommitClicked();
    void onCommitAndPushClicked();
    void onStageSelected();
    void onUnstageSelected();
    void onDiscardSelected();
    void onCommitComplete(bool success, const QString& message);
    void onOperationFailed(const QString& op, const QString& err);
    void updateTitle();

private:
    void setupUi();
    void wireConnections();
    void validateGeometryOnScreen();
    void showDiffForUnstaged(const QString& path);
    void showDiffForStaged(const QString& path);
    void clearDiff();
    void updateStatusBar();
    void readCommitterFromConfig();
    /// Amend is off, with the reason as its tooltip, while the
    /// repository has a merge, cherry-pick or revert to commit.
    void updateAmend();
    /// Where a merge, cherry-pick or revert waits for its commit, the
    /// message starts as the one git prepared for it
    /// (git::Repository::preparedMessage()), as a terminal's `git commit`
    /// does: the commit's own `-F -` took the typed words alone, and a
    /// revert's "This reverts commit <id>." went. Only into an empty
    /// message, or one still as it was filled in (git's for another
    /// stop, or another repository), which goes when there's none now.
    /// When the dialog is shown, and while it is open at every state
    /// report (an abort, a Skip, a merge started meanwhile), there
    /// `onlyIfChanged`: only when git's message is no longer the one
    /// last read, so a message the user cleared stays clear.
    void startFromPreparedMessage(bool onlyIfChanged = false);

    /// What can be clicked while a commit, or a local operation, runs:
    /// nothing that stages, unstages, discards or commits. git commit
    /// reads the index again after the pre-commit hook, so a file
    /// staged while the hook ran went into the commit unchecked, and an
    /// Unstage All made it empty; a second Commit (Ctrl+Return) started
    /// a second git commit.
    void applyLocks();
    /// A step runs on the repository (GitService::rebaseStepRunning()):
    /// a commit, this dialog's or another's, a rebase step, an abort.
    bool stepRunning() const;

    QStringList selectedUnstagedPaths() const;
    QStringList selectedStagedPaths() const;

    services::GitService*       svc_      = nullptr;
    conf::SettingsService*      settings_ = nullptr;

    // Splitter chain
    QSplitter*                  mainSplitter_  = nullptr;
    QSplitter*                  leftSplitter_  = nullptr;
    QSplitter*                  rightSplitter_ = nullptr;

    // File list models + filtering proxies
    models::FileStatusModel*    unstagedModel_ = nullptr;
    models::FileStatusModel*    stagedModel_   = nullptr;
    QSortFilterProxyModel*      unstagedProxy_ = nullptr;
    QSortFilterProxyModel*      stagedProxy_   = nullptr;

    // Unstaged section
    QLabel*                     unstagedLabel_  = nullptr;
    QLineEdit*                  unstagedFilter_ = nullptr;
    QListView*                  unstagedView_   = nullptr;
    QPushButton*                stageBtn_       = nullptr;
    QPushButton*                stageAllBtn_    = nullptr;
    QPushButton*                discardBtn_     = nullptr;

    // Staged section
    QLabel*                     stagedLabel_   = nullptr;
    QLineEdit*                  stagedFilter_  = nullptr;
    QListView*                  stagedView_    = nullptr;
    QPushButton*                unstageBtn_    = nullptr;
    QPushButton*                unstageAllBtn_ = nullptr;

    // Right pane
    widgets::DiffViewerWidget*  diffView_      = nullptr;

    // Commit controls (bottom-right)
    widgets::CommitMessageEdit* messageEdit_   = nullptr;
    QPushButton*                commitBtn_     = nullptr;
    QPushButton*                commitPushBtn_ = nullptr;
    QCheckBox*                  amendCheck_    = nullptr;
    QShortcut*                  commitShortcut_= nullptr;

    // Footer status strip
    QLabel*                     committerLabel_   = nullptr;
    QLabel*                     branchStatusLabel_= nullptr;
    QLabel*                     stagedCountLabel_ = nullptr;

    // Inline error surface (above status strip)
    QLabel*                     errorLabel_    = nullptr;

    // Cached state
    QString  repoName_;
    QString  currentBranch_;
    int      lastUnstagedCount_ = 0;
    int      lastStagedCount_   = 0;

    // Commit completion handshake
    bool pendingCommit_ = false;
    bool pushAfterCommit_ = false;
    // GitService::openKey() of the repository the pending commit is for.
    std::string pendingKey_;
    // setLocalOpBusy()
    bool localOpBusy_ = false;
    // applyLocks(): nothing stages, unstages or discards (a commit's
    // hooks, a local op or a step run); the lists and the diff stay live.
    bool stagingLocked_ = false;

    // Geometry restore guard
    bool restored_ = false;

    // startFromPreparedMessage(): the message it filled in last, as
    // the editor holds it (CRLF as "\n", a non-breaking space as a
    // space), and git's message as it read it last.
    QString prefilled_;
    QString preparedRead_;
};

} // namespace gitbolt::dialogs
