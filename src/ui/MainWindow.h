#pragma once

#include "git/Branch.h"
#include "git/Commit.h"
#include "git/Status.h"
#include "telemetry/TelemetryGate.h"

#include <QElapsedTimer>
#include <QFuture>
#include <QHash>
#include <QKeySequence>
#include <QList>
#include <QMainWindow>
#include <QPointer>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
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
namespace gitbolt::services { struct StopFacts;     }
namespace gitbolt::widgets  { class RepoOperationBar; class GitNoticeBar; }
namespace gitbolt::git      { struct GitProbe; }
namespace gitbolt::conf     { class SettingsService;}
namespace gitbolt::conf     { class ThemeService;   }
namespace gitbolt::dialogs  { class CommitDialog;   }
namespace gitbolt::dialogs  { class CloneDialog;    }
namespace gitbolt::telemetry { class TelemetryService; }

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

    /// Hand over main()'s usage statistics client and the gate it was
    /// created for (TelemetryGate.h). `service` is null when this
    /// session has none: compiled out, a test session, or turned off
    /// through the environment. Call once, before show(). With a
    /// service: menu and toolbar actions are counted by objectName,
    /// repository opens are timed, error kinds are reported, and the
    /// Settings switch reaches the service through THIS window's
    /// SettingsService — the one the Settings dialog writes through;
    /// settingsChanged is per instance, so main()'s would never hear
    /// it.
    void setTelemetry(telemetry::TelemetryService* service, telemetry::Gate gate);

    /// The gate main() evaluated: the Settings dialog shows the Usage
    /// statistics group unless it is CompiledOut, and the test bridge
    /// reports it in dump-state.
    telemetry::Gate telemetryGate() const;

    /// Finds the git GitBolt runs and asks it its version, on a worker
    /// (git::probeGit()), then shows what there is to say about it
    /// (showGitNotice). main() calls it once the window is up.
    void checkGitVersion();

    /// The note above the views about `probe` (widgets::GitNoticeBar),
    /// unless there is nothing to say or the user dismissed that one.
    void showGitNotice(const git::GitProbe& probe);

protected:
    void closeEvent(QCloseEvent* e) override;

private slots:
    void openRepository();
    void cloneRepository();
    void onRepositoryOpened(const QString& path);
    void onRepositoryOpenFailed(const QString& path, const QString& error, int errorCode);
    void onLogReady(std::vector<gitbolt::git::CommitData> commits, int offset);
    void onBranchesReady(std::vector<gitbolt::git::BranchInfo> branches);
    void showAbout();
    void showCommitDialog();
    void showSettingsDialog();
    void createNewRepository();

private:
    /// Tools › GitK in the open repository, with `childPath` as its PATH
    /// (util::pathForChildren()).
    void launchGitk(const QString& childPath);
    /// Paints the inline toolbar label / status bar / clear-timer
    /// after a fetch-pull-push worker finishes.
    void finishRemoteOpFeedback(const QString& successMsg);

    /// Run a network git op (fetch/pull/push/remote-branch delete) on a
    /// pool thread with inline-indicator + status-bar narration. `op`
    /// returns whether it succeeded (GitService's remote ops do).
    /// `after` runs on the GUI thread once the op finishes, success or
    /// failure.
    void runRemoteOp(QAction* sourceAction, const QString& startMsg,
                     const QString& successMsg, std::function<bool()> op,
                     std::function<void()> after = {});

    /// The part of a remote op that every caller shares: marks it
    /// running and runs `op` on a pool thread as remoteOp_ (which the
    /// destructor cancels and waits for); `finished` runs on the GUI
    /// thread afterwards, with lastRemoteOpFailed_ set from what `op`
    /// returned. Callers check remoteOpRunning_ first.
    void startRemoteOp(std::function<bool()> op,
                       std::function<void()> finished);

    /// Periodic fetch tick: a quiet fetch through startRemoteOp().
    void runPeriodicFetch();
    /// Turn a quiet auto-fetch into a narrated one, for a user remote
    /// op that has to wait for it. False when there's none to reveal.
    bool revealAutoFetch();
    /// Auto-fetch results stop being wanted (another repository is
    /// opened, the repo is closed, or the feature is switched off): a
    /// quiet auto-fetch still running is stopped and reports nothing,
    /// and the failure flag is cleared.
    void disownAutoFetch();
    /// The repository a Fetch click revealed an auto-fetch on has gone
    /// (closed, or replaced by another one): that fetch is stopped too,
    /// with "Fetch cancelled." where it was narrated.
    void cancelRevealedAutoFetch();
    /// A remote op held for a disowned auto-fetch won't be wanted when
    /// that one ends (the repo is closed, or another one is opening):
    /// it is dropped before it starts.
    void dropHeldRemoteOp();
    /// The end of a remote op that disabled `action`: enables it again
    /// if the repo actions are on (setRepoActionsEnabled); if they are
    /// off, whatever turns them on brings it back.
    void reenableAfterRemoteOp(QAction* action);
    /// Whether the inline label narrates a running remote op (a quiet
    /// auto-fetch is no such op): a local op then leaves it be.
    bool remoteOpNarrated() const;
    /// A local op that started under a narrated remote op, and outlived
    /// it: its message goes back on the label (unless a failure is
    /// sticky there) and the status bar, a while after the remote op's
    /// outcome went up (finishRemoteOpFeedback).
    void resumeLocalOpNarration();

    /// Commands > Merge branches and the sidebar's Merge: `git merge
    /// <branch>` as a local op (repoCmd_), offering the conflict
    /// resolver if git stopped on conflicts.
    void mergeBranch(const QString& branch);

    /// The sidebar's "Checkout as local branch" on `remoteBranch`
    /// ("origin/x"): checks out local "x", created to track it, as a
    /// local op (repoCmd_), unless it exists.
    void checkoutAsLocalBranch(const QString& remoteBranch);

    /// Confirm with the user, then delete `remoteBranch` ("origin/x")
    /// on its remote through runRemoteOp, if the repository open is
    /// still the one with `key` (taken before any picker that came
    /// first: GitService::openKey(), copied; the reference it returns
    /// follows the open repository).
    void confirmAndDeleteRemoteBranch(const QString& remoteBranch,
                                      QAction* sourceAction,
                                      std::string key);


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
    /// For entry points that run git or libgit2 on the repository
    /// straight away (the sidebar's checkout, branch create and delete,
    /// push): while a local op runs, or a step of the service's
    /// (gitStepRunning()), says so on the status bar, as
    /// RepoCommandController's refusal does, and returns true. The
    /// menus and toolbar are off then; these weren't, and a libgit2
    /// checkout ran under a merge.
    bool refuseWhileLocalOpRuns();
    /// For a local op that hands git branch, remote or revision names
    /// (`names`, from the repository or typed by the user): whether one
    /// of them is a name git would read as an option
    /// (git::GitProcess::checkOperand). Says so in a warning titled
    /// `failTitle`, as the op's own failure would be, and returns true;
    /// the caller then runs nothing.
    bool refuseOptionLikeNames(const QString& failTitle, const QStringList& names);
    /// A step runs on the open repository (GitService::
    /// rebaseStepRunning()): a commit and its hooks, a rebase step, an
    /// abort. The repo actions and menus are off then, as for a local
    /// op: a toolbar branch switch during a commit's pre-commit hook
    /// made the commit land on the other branch.
    bool gitStepRunning() const;
    /// Applies localOpRunning_ and gitStepRunning() to the repo actions,
    /// menus and sidebar again, keeping what repoActionsEnabled_ says.
    void reapplyRepoActionLocks();
    /// For a flow that asks first (a question, an editor, a picker) and
    /// then runs git on the open repository: whether that is no longer
    /// the repository it asked about (`key`, GitService::openKey() taken
    /// before asking) — another was opened, or is opening, meanwhile (a
    /// second launch hands its path over while the question is up).
    /// Says so on the status bar, `notDone` saying what wasn't done.
    /// Asked once the answer is in: git ran on whatever repository was
    /// open then, and Edit patterns applied one repository's sparse
    /// patterns to another, whose files went.
    bool repositoryChangedSince(const std::string& key, const QString& notDone);
    /// Closes `window`, a modeless one of the repository with `key`,
    /// once another repository is open (or none is).
    void closeWithRepository(QWidget* window, const std::string& key);
    /// For the in-progress bar's Skip and Abort, and its Continue of a
    /// paused sequence (`continuing`): finds the stop's
    /// services::StopFacts on a worker, the bar busy meanwhile, then
    /// calls `ask` with them and the open repository's key (for
    /// repositoryChangedSince() after its question), if that repository
    /// is still open and nothing else has started on it.
    void inspectStopThen(
        widgets::RepoOperationBar* bar,
        std::function<void(const services::StopFacts&, const std::string&)> ask,
        bool continuing = false);
    /// Keeps `apply` told whether the repository is busy (a local op or
    /// a step runs), for as long as `context` lives: the modeless
    /// windows' buttons (stashes, submodules) went to git under the op.
    void followRepoBusy(QObject* context, std::function<void(bool busy)> apply);

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

    /// Counts every menu and toolbar action's trigger by objectName
    /// (TelemetryService::countCommand) — never by text or bridge
    /// slug, which can hold paths and branch names. Each action once
    /// (the menu and the toolbar share QActions); Recent Repositories
    /// entries, whose texts are paths, all count as "file.recent".
    void instrumentActionsForTelemetry();
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
    /// via GitService::refreshConflicts; resolving stages + writes
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
    //
    // Usage statistics' repo_opened times an open from its start to
    // repositoryOpened (openedAfterMs) and on to its first history
    // page. GitService tags that page with the open's generation
    // (initialLogLoaded): a page of the previous repository, from a
    // refresh still walking when the open landed, arrives after
    // repositoryOpened too and must never be timed instead.
    struct PendingOpen {
        QString  path;                 // non-empty while in flight
        QWidget* widgetBefore = nullptr;  // revert target on failure
        QString  titleBefore;
        QString  pathLabelBefore;     // the status bar's path, restored too
        // The repository actions were on: a repository was on screen,
        // or File > Home left its actions on. Not GitService::isOpen(),
        // which Close leaves true.
        bool     actionsBefore = false;
        bool     awaitingInitialLog = false;  // spinner latch
        quint64       generation = 0;     // GitService::openRepositoryAsync's
        QElapsedTimer started;            // invalid: nothing being timed
        qint64        openedAfterMs = -1; // -1 until repositoryOpened

        bool active() const { return !path.isEmpty(); }
        void clear() {
            path.clear();
            widgetBefore = nullptr;
            titleBefore.clear();
            pathLabelBefore.clear();
            actionsBefore = false;
            awaitingInitialLog = false;
            stopTiming();
        }
        void stopTiming() {
            started.invalidate();
            openedAfterMs = -1;
        }
    } pendingOpen_;

    QStackedWidget* centralStack_  = nullptr;
    DashboardView*  dashboardView_ = nullptr;
    RepositoryView* repoView_      = nullptr;
    widgets::GitNoticeBar* gitNotice_ = nullptr;

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
    // Its repository actions are off with the others'; Periodic
    // background fetch (a setting) and Plugin Manager stay on.
    QMenu*     pluginsMenu_    = nullptr;
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

    // Plugins → Periodic background fetch. Constructed with the
    // Plugins menu and kept for the window's lifetime; it runs only
    // while the feature is on. Named "periodicFetchTimer" for the
    // test bridge's fire-timer.
    QTimer* periodicFetchTimer_ = nullptr;
    QLabel* periodicFetchStatus_ = nullptr;
    // What the running remote op is when the periodic fetch started
    // it (see runPeriodicFetch). Quiet: reports to the status bar and
    // the auto-fetch label only. Shown: a user's remote op revealed
    // it, so it reports like a toolbar Fetch. Disowned: its result is
    // no longer wanted (disownAutoFetch, cancelRevealedAutoFetch), so
    // its git is stopped and it reports nothing.
    enum class AutoFetch { None, Quiet, Shown, Disowned };
    AutoFetch autoFetch_ = AutoFetch::None;
    // The running auto-fetch's own stop flag (GitService::fetch's
    // `cancel`), which disowning it sets.
    std::shared_ptr<std::atomic<bool>> autoFetchCancel_;
    // The key (GitService::openKey(), its git dir) of the repository
    // the running auto-fetch fetches, for onRepositoryOpened to tell
    // whether a revealed one's is gone.
    std::string autoFetchRepo_;
    // A remote op clicked while a disowned auto-fetch wound down
    // (runRemoteOp); that fetch's finished handler starts it.
    std::function<void()> heldRemoteOp_;
    // git's message from the last failed auto-fetch, flagged on
    // periodicFetchStatus_ until one succeeds.
    QString autoFetchError_;
    // The running auto-fetch has failed, as a quiet one: its failure is
    // in, so a Fetch click no longer reveals it (revealAutoFetch).
    bool autoFetchFailed_ = false;
    // The repository key (autoFetchRepo_) of a revealed auto-fetch
    // that ended while an open was pending: its outcome went up over
    // the loading screen, and goes once another repository is open
    // (onRepositoryOpened).
    std::string revealedFetchEndedRepo_;

    // Whether the last remote op failed: its own outcome, from what
    // its op returned (startRemoteOp). If true, the failed-message
    // that the operationFailed handler put on the status bar stays;
    // otherwise we show our own confirmation message.
    bool lastRemoteOpFailed_ = false;
    bool remoteOpRunning_ = false;
    // The action that started the running remote op (runRemoteOp): it
    // stays off until the op ends, whatever turns the others on.
    QAction* remoteOpAction_ = nullptr;
    // What setRepoActionsEnabled last set: whether there's a repository
    // for the repo actions (and the repo-only menus, which go with
    // them) to act on. A remote op's end re-enables its action only
    // then (reenableAfterRemoteOp).
    bool repoActionsEnabled_ = false;
    // The open repository is bare: Pull and Commit, which need a work
    // tree, stay off (git: "this operation must be run in a work tree").
    bool openIsBare_ = false;
    // The running (or last) remote op's pool-thread job: the destructor
    // waits for it, since it runs inside gitService_.
    QFuture<bool> remoteOp_;
    // A local op (repoCmd_) is running: the repo actions and menus are
    // off until it ends. What it says it does, and whether it says so
    // on the inline label (not over a remote op's narration).
    bool localOpRunning_ = false;
    // A repoStateReady came while a local op ran, and the in-progress
    // bar didn't take it: a status refresh follows the op's end.
    bool barStateHeld_ = false;
    QString localOpMessage_;
    bool localOpNarrated_ = false;
    // Why the open repository's status can't be read, as the status bar
    // said it last; empty while it reads (statusReady), and for a
    // repository just opened.
    QString statusFailure_;
    // GitService::stepWarning of the step about to report: git's
    // warnings on a step that went through.
    QString stepWarning_;
    // The bar's Abort of a cherry-pick or revert sequence paused after a
    // commit made during it, which git stops without rewinding: its
    // outcome says so.
    bool abortKeepsCommits_ = false;
    // A Skip or an Abort of the in-progress bar is looking at the stop
    // (inspectStopThen()); each look, and each open, a generation.
    bool inspectingStop_ = false;
    quint64 stopInspection_ = 0;

    // Real shortcut registry: every named leaf menu action, with the
    // shortcut it was constructed with. Built by
    // collectAndApplyShortcuts(); consumed by the Settings dialog.
    // Guarded: an action deleted after startup must drop out, not
    // reach the dialog as a dangling pointer.
    QList<QPointer<QAction>> shortcutActions_;
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
    // shell out to git: local ops, on a pool thread, one at a time.
    // The destructor waits for the running one.
    RepoCommandController* repoCmd_ = nullptr;

    // Usage statistics (setTelemetry). Owned by main(), declared there
    // before the window, so it outlives this pointer.
    telemetry::TelemetryService* telemetry_ = nullptr;
    telemetry::Gate telemetryGate_ = telemetry::Gate::CompiledOut;
};

} // namespace gitbolt::ui
