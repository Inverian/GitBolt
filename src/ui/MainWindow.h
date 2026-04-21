#pragma once

#include "git/Branch.h"
#include "git/Commit.h"
#include "git/Status.h"

#include <QMainWindow>
#include <vector>

class QStackedWidget;
class QLabel;
class QLineEdit;
class QMenu;
class QAction;

namespace gitbolt::models   { class CommitLogModel; }
namespace gitbolt::services { class GitService;     }
namespace gitbolt::conf     { class SettingsService;}
namespace gitbolt::conf     { class ThemeService;   }
namespace gitbolt::dialogs  { class CommitDialog;   }
namespace gitbolt::dialogs  { class CloneDialog;    }

namespace gitbolt::ui {

class RepositoryView;
class DashboardView;

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
    void onLogReady(std::vector<gitbolt::git::CommitData> commits, int offset);
    void onBranchesReady(std::vector<gitbolt::git::BranchInfo> branches);
    void showAbout();
    void showCommitDialog();
    void showSettingsDialog();

private:
    void createMenuBar();
    void createToolBar();
    /// Enable or disable every child action under the Navigate,
    /// View, and Commands menus. Called with `false` when no repo
    /// is open (home screen / after Close) and `true` after a
    /// repo is successfully opened.
    void setRepoOnlyMenusEnabled(bool on);
    void createStatusBar();
    void setupConnections();
    void updateRecentMenu();
    void persistLayout();

    services::GitService*   gitService_   = nullptr;
    conf::SettingsService*  settingsService_ = nullptr;
    conf::ThemeService*     themeService_    = nullptr;  // owned by main()
    models::CommitLogModel* commitLogModel_  = nullptr;

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
    QMenu*     navMenu_        = nullptr;
    QMenu*     viewMenu_       = nullptr;
    QMenu*     cmdMenu_        = nullptr;
    QAction*   refreshAction_  = nullptr;  // repo-dependent
    QAction*   fetchAction_    = nullptr;  // repo-dependent
    QAction*   pullAction_     = nullptr;  // repo-dependent
    QAction*   pushAction_     = nullptr;  // repo-dependent
    QAction*   commitAction_   = nullptr;  // repo-dependent
    QAction*   stashAction_    = nullptr;  // toolbar Stash (placeholder)
    QAction*   settingsAction_ = nullptr;  // toolbar Settings (placeholder)
    QLineEdit* filterInput_    = nullptr;  // toolbar quick filter (placeholder)
};

} // namespace gitbolt::ui
