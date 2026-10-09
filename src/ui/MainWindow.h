#pragma once

#include "git/Branch.h"
#include "git/Commit.h"
#include "git/Status.h"

#include <QFuture>
#include <QHash>
#include <QKeySequence>
#include <QList>
#include <QMainWindow>
#include <functional>
#include <vector>

class QStackedWidget;
class QComboBox;
class QLabel;
class QLineEdit;
class QMenu;
class QAction;
class QTimer;

namespace gitbolt::models   { class CommitLogModel; }
namespace gitbolt::services { class GitService;     }
namespace gitbolt::conf     { class SettingsService;}
namespace gitbolt::conf     { class ThemeService;   }
namespace gitbolt::dialogs  { class CommitDialog;   }
namespace gitbolt::dialogs  { class CloneDialog;    }

namespace gitbolt::ui {

class RepositoryView;
class DashboardView;
class InlineOpIndicator;
class RepoCommandController;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    /// Open a repository by path. Used by main() to handle the
    /// command-line argument and by recent-repo menu actions.
    void openRepositoryAtPath(const QString& path);

    /// Inject the app-wide ThemeService so the Settings dialog's
    /// Appearance page can list and switch themes. main() already
    /// owns the ThemeService; MainWindow doesn't create its own
    /// to avoid two instances fighting over the palette.
    void setThemeService(conf::ThemeService* theme);

protected:
    void closeEvent(QCloseEvent* e) override;

private slots:
    void openRepository();
    void cloneRepository();
    void onRepositoryOpened(const QString& path);
    void onRepositoryOpenFailed(const QString& path, const QString& error);
    void onLogReady(std::vector<gitbolt::git::CommitData> commits, int offset);
    void onBranchesReady(std::vector<gitbolt::git::BranchInfo> branches);
    void showAbout();
    void showCommitDialog();
    void showSettingsDialog();
    void createNewRepository();

private:
    /// Paints the inline toolbar label / status bar / clear-timer
    /// after a fetch-pull-push worker finishes.
    void finishRemoteOpFeedback(const QString& successMsg);

    /// Run a network git op (fetch/pull/push/remote-branch delete) on a
    /// pool thread with inline-indicator + status-bar narration. `after`
    /// runs on the GUI thread once the op finishes, success or failure.
    void runRemoteOp(QAction* sourceAction, const QString& startMsg,
                     const QString& successMsg, std::function<void()> op,
                     std::function<void()> after = {});

    /// Confirm with the user, then delete `remoteBranch` ("origin/x")
    /// on its remote through runRemoteOp.
    void confirmAndDeleteRemoteBranch(const QString& remoteBranch,
                                      QAction* sourceAction);


private:
    void createMenuBar();
    // Per-menu builders — one function per top-level menu, called in
    // menu-bar order by createMenuBar(). Pure structure; handlers
    // they declare delegate to GitService / repoCmd_ helpers.
    void buildFileMenu();
    void buildRepositoryMenu();
    void buildNavigateMenu();
    void buildViewMenu();
    void buildCommandsMenu();
    void buildPluginsMenu();
    void buildToolsMenu();
    void buildHelpMenu();
    void createToolBar();
    /// Enable or disable every child action under the Navigate,
    /// View, and Commands menus. Called with `false` when no repo
    /// is open (home screen / after Close) and `true` after a
    /// repo is successfully opened.
    void setRepoOnlyMenusEnabled(bool on);
    /// Enable or disable the repo-dependent toolbar/menu actions
    /// (refresh, fetch, pull, push, commit, filter, branch combo).
    /// Disabled on the home screen, after Close, and while an
    /// async repository open is in flight (so Pull/Commit can't
    /// fire against the previous repo mid-open).
    void setRepoActionsEnabled(bool on);

    /// Walk every named leaf action under the menu bar, record its
    /// factory-default shortcut, and apply any user override saved
    /// under "shortcuts/<objectName>". Runs once at the end of
    /// createMenuBar(); the same list feeds the Settings dialog's
    /// Shortcuts page, which is how the page edits REAL bindings
    /// instead of a hand-typed placebo list.
    void collectAndApplyShortcuts();

    /// Recursively assign a stable objectName to every leaf action
    /// under `widget` (a menu bar, menu, or toolbar) that doesn't
    /// already have one, derived from `pathPrefix` + the action's
    /// text (e.g. "commands.resolve-conflicts"). Guarantees the
    /// test bridge can address every action — see the convention
    /// note in CONTRIBUTING.md and docs/AGENT_TESTING.md.
    void assignActionObjectNames(QWidget* widget, const QString& pathPrefix);
    void createStatusBar();
    void setupConnections();
    void updateRecentMenu();
    void persistLayout();

    /// Refresh the right-side status-bar label that shows whether
    /// the Periodic fetch plugin is currently active. Called after
    /// every toggle and start/stop of the timer.
    void updatePeriodicFetchStatus();

    /// Push the current commit hash onto the back-history stack,
    /// invoked when the user navigates to a NEW commit (not via the
    /// back/forward buttons). Clears the forward stack — same model
    /// browsers use: a fresh navigation invalidates the redo path.
    void pushHistory(const QString& commitHash);

    /// Open the three-way conflict resolver dialog (hosting
    /// MergeConflictWidget). Reads the index's conflicted entries
    /// via GitService::refreshConflicts; resolving writes + stages
    /// each file, aborting runs the state-appropriate --abort.
    /// Reached from Commands → Resolve conflicts and from the
    /// offer shown after a merge / cherry-pick hits conflicts.
    void showConflictResolver();

    /// If the repository is mid-merge/cherry-pick/rebase after a
    /// failed operation, offer to open the conflict resolver.
    void offerConflictResolution(const QString& operation);

    services::GitService*   gitService_   = nullptr;
    conf::SettingsService*  settingsService_ = nullptr;
    conf::ThemeService*     themeService_    = nullptr;  // owned by main()
    models::CommitLogModel* commitLogModel_  = nullptr;

    // Async-open state. pendingOpen_.path is non-empty while a
    // GitService::openRepositoryAsync is in flight; the snapshot
    // pair records where the user was before the optimistic switch
    // to the repo view so a failed open can put them back (only the
    // FIRST open of a burst snapshots — a second open while one is
    // pending must not capture the loading screen itself).
    // pendingOpen_.awaitingInitialLog latches the spinner: armed on open
    // and cleared by the first logReady(offset==0), which is
    // guaranteed to arrive (refreshLog emits an empty page even on
    // walk errors).
    // The whole in-flight-open record lives in one value so the
    // begin (openRepositoryAtPath), success (onRepositoryOpened),
    // and failure (onRepositoryOpenFailed) handlers can't half-
    // update it. This is the session state the review's
    // "RepoSessionController" sketch wanted isolated; the handlers
    // stay on MainWindow because they ARE window manipulation
    // (title, central stack, model clears).
    struct PendingOpen {
        QString  path;                 // non-empty while in flight
        QWidget* widgetBefore = nullptr;  // revert target on failure
        QString  titleBefore;
        bool     awaitingInitialLog = false;  // spinner latch

        bool active() const { return !path.isEmpty(); }
        void clear() {
            path.clear();
            widgetBefore = nullptr;
            titleBefore.clear();
            awaitingInitialLog = false;
        }
    } pendingOpen_;

    QStackedWidget* centralStack_  = nullptr;
    DashboardView*  dashboardView_ = nullptr;
    RepositoryView* repoView_      = nullptr;

    // Modeless commit dialog — lazily constructed on first use and
    // then cached. Never deleted before the window closes.
    dialogs::CommitDialog* commitDialog_ = nullptr;

    // Status bar labels
    QLabel* branchLabel_   = nullptr;
    QLabel* repoPathLabel_ = nullptr;

    // Menus / actions
    //
    // The actions below are constructed ONCE and added to both the
    // toolbar and the Repository menu via addAction(QAction*). That
    // way a single setEnabled() call toggles the toolbar button AND
    // the menu item in lockstep — the "same action, multiple entry
    // points" idiomatic Qt pattern. All repo-dependent actions are
    // constructed in a disabled state and re-enabled inside
    // onRepositoryOpened().
    QMenu*     recentMenu_     = nullptr;
    // Top-level menus that are only meaningful when a repository is
    // open. Disabled (grayed out) on the home/dashboard screen and
    // re-enabled in onRepositoryOpened(). Toggling the menu's
    // QAction (via menuAction()) grays out the menu title itself
    // AND prevents the dropdown from opening.
    QMenu*     repoMenu_       = nullptr;
    QMenu*     navMenu_        = nullptr;
    QMenu*     viewMenu_       = nullptr;
    QMenu*     cmdMenu_        = nullptr;
    QAction*   refreshAction_  = nullptr;  // repo-dependent
    QAction*   fetchAction_    = nullptr;  // repo-dependent
    QAction*   pullAction_     = nullptr;  // repo-dependent
    QAction*   pushAction_     = nullptr;  // repo-dependent
    QAction*   commitAction_   = nullptr;  // repo-dependent
    QAction*   stashAction_    = nullptr;  // shared menu "Manage
                                           // stashes..." + toolbar Stash
    QLineEdit* filterInput_    = nullptr;  // toolbar quick filter (placeholder)
    // The "Filter:" label and its input belong to the Repository
    // workflow — irrelevant on the home screen. Hidden initially
    // and shown when a repo opens (onRepositoryOpened), hidden
    // again on Close. Stored as QActions because QToolBar
    // addWidget() returns them and setVisible() on the action is
    // what actually hides the wrapped widget in the toolbar.
    QAction*   filterLabelAction_ = nullptr;
    QAction*   filterInputAction_ = nullptr;

    // Quick-switch branch dropdown that lives on the toolbar.
    // Populated from each branchesReady signal with the list of
    // local branches, current branch pre-selected. Choosing a
    // different entry triggers checkoutBranch(); the next
    // branchesReady refresh repaints the selection. Hidden until
    // a repo opens (mirroring filterInput_).
    QComboBox* branchCombo_      = nullptr;
    QAction*   branchLabelAction_ = nullptr;
    QAction*   branchComboAction_ = nullptr;

    // Browser-style commit history. backHistory_ holds the commits
    // BEFORE the current one (top = most recent visited); the
    // currently-selected commit lives in `currentNavCommit_`;
    // forwardHistory_ is populated by pressing Back. A fresh
    // navigation (the user clicks a row, or selects via Go to
    // commit / current revision) clears forwardHistory_. The
    // suppress flag distinguishes selection signals fired by
    // pushHistory's own selectCommit calls from genuine new
    // navigation, so back/forward don't mangle their own stacks.
    QStringList backHistory_;
    QStringList forwardHistory_;
    QString     currentNavCommit_;
    bool        suppressHistoryPush_ = false;

    // (action, columnIndex) for each View menu column-visibility
    // toggle. Re-applied to the revision graph after a repo opens
    // so saved hidden columns stay hidden across restarts.
    struct ColumnToggle { QAction* action; int columnIndex; };
    std::vector<ColumnToggle> columnToggles_;

    // (action, branchModel category index) for branch-tree section
    // toggles. Same pattern as columnToggles_ — restored from
    // settings at construction, re-applied after the repo opens
    // because the model is empty until then.
    struct BranchTreeToggle { QAction* action; int categoryIndex; };
    std::vector<BranchTreeToggle> branchTreeToggles_;

    // Plugins → Periodic background fetch. Lazily constructed when
    // the user enables the feature; kept alive for the window's
    // lifetime so toggling off/on doesn't lose the connection.
    QTimer* periodicFetchTimer_ = nullptr;
    QLabel* periodicFetchStatus_ = nullptr;

    // Set true by the operationFailed handler when fetch/pull/push
    // emit a failure. Read by the toolbar action handlers right
    // after the synchronous git op returns: if the flag is still
    // false the op succeeded silently and we show our own
    // confirmation message; if true, the failed-message that the
    // operationFailed handler put on the status bar stays.
    bool lastRemoteOpFailed_ = false;
    bool remoteOpRunning_ = false;
    // The running (or last) remote op's pool-thread job: the destructor
    // waits for it, since it runs inside gitService_.
    QFuture<void> remoteOp_;

    // Real shortcut registry: every named leaf menu action, with the
    // shortcut it was constructed with. Built by
    // collectAndApplyShortcuts(); consumed by the Settings dialog.
    QList<QAction*> shortcutActions_;
    QHash<QString, QKeySequence> defaultShortcuts_;

    // Inline activity indicator that lives on the toolbar between the
    // push button and the Commit button. Shown in italic-color text
    // while a fetch/pull/push is running, and again in green/red for
    // a few seconds after it completes — gives the user noticeable
    // feedback about whether the click did anything, since the status
    // bar at the bottom of the window is easy to miss.
    QLabel*  remoteOpLabel_      = nullptr;
    QAction* remoteOpLabelAction_ = nullptr;
    // Inline toolbar op narration (start/succeed/fail/flash) — owns
    // the auto-clear timer that three sites used to hand-roll.
    InlineOpIndicator* opIndicator_ = nullptr;
    // Shared guard→run→report→refresh spine for menu actions that
    // shell out to git.
    RepoCommandController* repoCmd_ = nullptr;
};

} // namespace gitbolt::ui
