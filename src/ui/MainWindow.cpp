#include "ui/MainWindow.h"
#include "ui/RepositoryView.h"
#include "ui/DashboardView.h"
#include "dialogs/AboutDialog.h"
#include "dialogs/CloneDialog.h"
#include "dialogs/CommitDialog.h"
#include "dialogs/SettingsDialog.h"
#include "models/CommitLogModel.h"
#include "services/GitService.h"
#include "conf/SettingsService.h"
#include "conf/ThemeService.h"
#include "widgets/BranchTreeWidget.h"
#include "widgets/RevisionGraphWidget.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QPointer>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QStackedWidget>
#include <QStatusBar>
#include <QToolBar>

namespace gitbolt::ui {

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("GitBolt"));
    resize(1280, 800);

    // --- Core services ---
    gitService_ = new services::GitService(this);
    settingsService_ = new conf::SettingsService(this);

    // --- Commit log model ---
    commitLogModel_ = new models::CommitLogModel(this);

    // --- Central stacked widget: Dashboard / Repository views ---
    centralStack_ = new QStackedWidget(this);
    dashboardView_ = new DashboardView(settingsService_, this);
    repoView_ = new RepositoryView(this);
    repoView_->setGitService(gitService_);
    repoView_->setCommitLogModel(commitLogModel_);
    repoView_->setSettingsService(settingsService_);

    centralStack_->addWidget(dashboardView_);
    centralStack_->addWidget(repoView_);
    setCentralWidget(centralStack_);

    createMenuBar();
    createToolBar();
    createStatusBar();
    setupConnections();

    // Persist splitter state on app teardown. closeEvent fires when
    // the user clicks the red traffic-light button on the window.
    // On macOS, Cmd+Q / Apple-menu Quit / AppleScript "tell app to
    // quit" use a different code path that does NOT fire closeEvent,
    // so we also hook aboutToQuit. Both paths funnel to the same
    // idempotent persistLayout() helper.
    QPointer<MainWindow> self = this;
    connect(qApp, &QCoreApplication::aboutToQuit, this, [self]() {
        if (self)
            self->persistLayout();
    }, Qt::DirectConnection);
}

MainWindow::~MainWindow() = default;

// ---------------------------------------------------------------------------
// Persist splitter state on window close. We intentionally do NOT
// save QMainWindow::saveState() anymore — the dock widgets it used
// to encode are gone, and the toolbar is non-movable.
// ---------------------------------------------------------------------------
void MainWindow::closeEvent(QCloseEvent* e)
{
    persistLayout();
    QMainWindow::closeEvent(e);
}

// Idempotent: safe to call from both closeEvent and aboutToQuit.
// QSplitter::saveState() is cheap and the bytes go straight into
// QSettings, which dedupes by key.
void MainWindow::persistLayout()
{
    if (!settingsService_ || !repoView_)
        return;
    settingsService_->saveSplitterState(
        QStringLiteral("repoSplitterH/v1"),
        repoView_->saveRepoSplitterH());
    settingsService_->saveSplitterState(
        QStringLiteral("repoSplitterV/v1"),
        repoView_->saveRepoSplitterV());
    settingsService_->saveSplitterState(
        QStringLiteral("diffSplitter/v1"),
        repoView_->saveDiffSplitter());
}

// ---------------------------------------------------------------------------
// Menu bar
// ---------------------------------------------------------------------------
//
// The menu structure mirrors the GitExtensions browse window:
//   Start | Repository | Navigate | View | Commands | GitHub |
//   Plugins | Tools | Help
//
// Many entries are still placeholders — they're present so users
// can see what's coming and so the menu chrome matches the
// reference UI. Stub items are added with addPlaceholder() and
// fire a "not yet implemented" status-bar message when clicked.
// ---------------------------------------------------------------------------

namespace {
// Add a placeholder action that does nothing user-facing, but
// emits a status-bar message so users know the slot exists.
QAction* addPlaceholder(QMenu* menu, const QString& label,
                        QStatusBar* status)
{
    auto* a = menu->addAction(label);
    QObject::connect(a, &QAction::triggered, [label, status]() {
        if (status)
            status->showMessage(
                QObject::tr("%1 — not yet implemented").arg(label),
                3000);
    });
    return a;
}
} // namespace

void MainWindow::createMenuBar()
{
    auto* status = statusBar();

    // Keep the menu bar INSIDE the window on every platform. By
    // default Qt on macOS moves QMainWindow::menuBar() into the
    // system-wide menu bar at the top of the screen, which would
    // leave Windows/Linux with a two-row header (menu + toolbar)
    // and macOS with a one-row header (toolbar only, menus up top).
    // Forcing setNativeMenuBar(false) keeps the layout identical
    // across platforms: menu row above the toolbar row, always.
    menuBar()->setNativeMenuBar(false);

    // ---- macOS-only: inject an "About GitBolt" entry into the
    // system-provided Application menu (the one with Services /
    // Hide / Quit). Because our in-window menu bar is non-native,
    // none of its actions reach the macOS system menu bar. The
    // classic Qt pattern for contributing items to the macOS app
    // menu independently of the window menu bar is to allocate a
    // PARENTLESS QMenuBar — the Cocoa backend treats it as the
    // app-wide fallback menu bar, and any action tagged with
    // AboutRole gets auto-hoisted to the top of the Application
    // menu alongside the OS's own entries. The parentless menu
    // bar intentionally lives for the lifetime of the process.
#ifdef Q_OS_MACOS
    {
        auto* globalBar  = new QMenuBar(nullptr);
        auto* appMenu    = globalBar->addMenu(QStringLiteral("App"));  // label unused
        auto* aboutAct   = appMenu->addAction(tr("About GitBolt"));
        aboutAct->setMenuRole(QAction::AboutRole);
        connect(aboutAct, &QAction::triggered, this, &MainWindow::showAbout);
    }
#endif

    // ---- File ----
    auto* fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->addAction(tr("&Open Repository..."), this,
                        &MainWindow::openRepository, QKeySequence::Open);
    fileMenu->addAction(tr("&Clone Repository..."), this,
                        &MainWindow::cloneRepository);
    addPlaceholder(fileMenu, tr("Create &New Repository..."), status);

    recentMenu_ = fileMenu->addMenu(tr("Recent Repositories"));
    updateRecentMenu();

    fileMenu->addSeparator();
    fileMenu->addAction(tr("&Quit"), qApp, &QApplication::quit, QKeySequence::Quit);

    // ---- Repository ----
    //
    // Refresh / Fetch / Pull / Push / Commit are created here as
    // shared QAction members so createToolBar() can add the SAME
    // instances to the toolbar — that way the disabled-without-repo
    // state is managed in exactly one place (onRepositoryOpened).
    auto* repoMenu = menuBar()->addMenu(tr("&Repository"));

    refreshAction_ = new QAction(tr("&Refresh"), this);
    refreshAction_->setShortcut(QKeySequence::Refresh);
    refreshAction_->setEnabled(false);
    connect(refreshAction_, &QAction::triggered, this, [this]() {
        if (!gitService_ || !gitService_->isOpen())
            return;
        gitService_->refreshStatus();
        gitService_->refreshLog();
        gitService_->refreshBranches();
    });
    repoMenu->addAction(refreshAction_);

    addPlaceholder(repoMenu, tr("File E&xplorer"), status)
        ->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O));

    repoMenu->addSeparator();
    addPlaceholder(repoMenu, tr("Remote &repositories..."), status);

    repoMenu->addSeparator();
    addPlaceholder(repoMenu, tr("Manage &submodules..."), status);
    addPlaceholder(repoMenu, tr("&Update all submodules"), status);
    addPlaceholder(repoMenu, tr("S&ynchronize all submodules"), status);

    repoMenu->addSeparator();
    addPlaceholder(repoMenu, tr("Manage &worktrees..."), status)
        ->setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_W));

    repoMenu->addSeparator();
    addPlaceholder(repoMenu, tr("Edit .&gitignore"), status);
    addPlaceholder(repoMenu, tr("Edit .git/&info/exclude"), status);
    addPlaceholder(repoMenu, tr("Edit .git&attributes"), status);
    addPlaceholder(repoMenu, tr("Edit .&mailmap"), status);
    addPlaceholder(repoMenu, tr("Sparse &Working Copy"), status);

    repoMenu->addSeparator();
    addPlaceholder(repoMenu, tr("Git mai&ntenance"), status);

    repoMenu->addSeparator();
    addPlaceholder(repoMenu, tr("Repository &settings..."), status);

    repoMenu->addSeparator();
    auto* closeAction = new QAction(tr("&Close (go to Dashboard)"), this);
    closeAction->setShortcut(QKeySequence::Close);
    connect(closeAction, &QAction::triggered, this, [this]() {
        centralStack_->setCurrentWidget(dashboardView_);
        setWindowTitle(QStringLiteral("GitBolt"));
    });
    repoMenu->addAction(closeAction);

    // ---- Navigate ----
    auto* navMenu = menuBar()->addMenu(tr("&Navigate"));
    addPlaceholder(navMenu, tr("Go to &current revision"), status)
        ->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C));
    addPlaceholder(navMenu, tr("Go to c&ommit..."), status)
        ->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_G));

    navMenu->addSeparator();
    addPlaceholder(navMenu, tr("Go to &child commit"), status)
        ->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_N));
    addPlaceholder(navMenu, tr("Go to &parent commit"), status)
        ->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_P));
    addPlaceholder(navMenu, tr("Go to &first parent commit"), status);
    addPlaceholder(navMenu, tr("Go to &last parent commit"), status);

    navMenu->addSeparator();
    addPlaceholder(navMenu, tr("Navigate &backward"), status)
        ->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Left));
    addPlaceholder(navMenu, tr("Navigate f&orward"), status)
        ->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Right));

    navMenu->addSeparator();
    addPlaceholder(navMenu, tr("Quick &search"), status);
    addPlaceholder(navMenu, tr("Quick search pre&vious"), status)
        ->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Up));
    addPlaceholder(navMenu, tr("Quick search ne&xt"), status)
        ->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Down));

    // ---- View ----
    auto* viewMenu = menuBar()->addMenu(tr("&View"));

    // -- Branches section --
    addPlaceholder(viewMenu, tr("Show &all branches"), status)
        ->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_A));
    addPlaceholder(viewMenu, tr("Show c&urrent branch only"), status)
        ->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_U));
    addPlaceholder(viewMenu, tr("Show &filtered branches"), status)
        ->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_T));
    addPlaceholder(viewMenu, tr("Show &reflog references"), status)
        ->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_L));

    viewMenu->addSeparator();
    addPlaceholder(viewMenu, tr("Ad&vanced filter..."), status)
        ->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_I));

    viewMenu->addSeparator();
    // -- Commits section --
    auto* showStashes = addPlaceholder(viewMenu, tr("Show s&tashes"), status);
    showStashes->setCheckable(true);
    showStashes->setChecked(true);
    addPlaceholder(viewMenu, tr("Show git &notes"), status)
        ->setCheckable(true);

    viewMenu->addSeparator();
    // -- Grid labels section --
    auto* showRemote = addPlaceholder(viewMenu, tr("Show &remote branches"), status);
    showRemote->setCheckable(true);
    showRemote->setChecked(true);
    showRemote->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_R));
    auto* showTags = addPlaceholder(viewMenu, tr("Show ta&gs"), status);
    showTags->setCheckable(true);
    showTags->setChecked(true);
    showTags->setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_T));

    viewMenu->addSeparator();
    // -- Grid info section --
    auto* showMsgBody = addPlaceholder(viewMenu, tr("Show commit &message body"), status);
    showMsgBody->setCheckable(true);
    showMsgBody->setChecked(true);
    auto* showAuthorDate = addPlaceholder(viewMenu, tr("Show a&uthor date"), status);
    showAuthorDate->setCheckable(true);
    showAuthorDate->setChecked(true);
    auto* showRelDate = addPlaceholder(viewMenu, tr("Show relati&ve date"), status);
    showRelDate->setCheckable(true);
    showRelDate->setChecked(true);

    viewMenu->addSeparator();
    // -- Columns section --
    auto* showGraph = addPlaceholder(viewMenu, tr("Show revision &graph column"), status);
    showGraph->setCheckable(true);
    showGraph->setChecked(true);
    auto* showAvatar = addPlaceholder(viewMenu, tr("Show author a&vatar column"), status);
    showAvatar->setCheckable(true);
    showAvatar->setChecked(true);
    auto* showAuthorName = addPlaceholder(viewMenu, tr("Show author &name column"), status);
    showAuthorName->setCheckable(true);
    showAuthorName->setChecked(true);
    auto* showDateCol = addPlaceholder(viewMenu, tr("Show &date column"), status);
    showDateCol->setCheckable(true);
    showDateCol->setChecked(true);
    auto* showHashCol = addPlaceholder(viewMenu, tr("Show SHA-&1 column"), status);
    showHashCol->setCheckable(true);
    showHashCol->setChecked(true);

    // ---- Commands ----
    auto* cmdMenu = menuBar()->addMenu(tr("&Commands"));

    commitAction_ = new QAction(tr("Co&mmit..."), this);
    commitAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Space));
    commitAction_->setEnabled(false);
    connect(commitAction_, &QAction::triggered,
            this, &MainWindow::showCommitDialog);
    cmdMenu->addAction(commitAction_);

    addPlaceholder(cmdMenu, tr("&Undo last commit..."), status);

    cmdMenu->addSeparator();

    fetchAction_ = new QAction(tr("Pull/&Fetch..."), this);
    fetchAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Down));
    fetchAction_->setEnabled(false);
    connect(fetchAction_, &QAction::triggered, this,
            [this]() { gitService_->fetch(); });
    cmdMenu->addAction(fetchAction_);

    pullAction_ = new QAction(tr("Pu&ll"), this);
    pullAction_->setEnabled(false);
    connect(pullAction_, &QAction::triggered, this,
            [this]() { gitService_->pull("origin", ""); });

    pushAction_ = new QAction(tr("&Push..."), this);
    pushAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Up));
    pushAction_->setEnabled(false);
    connect(pushAction_, &QAction::triggered, this,
            [this]() { gitService_->push("origin", ""); });
    cmdMenu->addAction(pushAction_);

    cmdMenu->addSeparator();
    addPlaceholder(cmdMenu, tr("Manage &stashes..."), status);
    addPlaceholder(cmdMenu, tr("&Reset changes..."), status);
    addPlaceholder(cmdMenu, tr("Clea&n working directory..."), status);

    cmdMenu->addSeparator();
    addPlaceholder(cmdMenu, tr("Create &branch..."), status)
        ->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_B));
    addPlaceholder(cmdMenu, tr("&Delete branch..."), status);
    addPlaceholder(cmdMenu, tr("Check&out branch..."), status);
    addPlaceholder(cmdMenu, tr("Mer&ge branches..."), status)
        ->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_M));
    addPlaceholder(cmdMenu, tr("R&ebase..."), status)
        ->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E));

    cmdMenu->addSeparator();
    addPlaceholder(cmdMenu, tr("Create &tag..."), status)
        ->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_T));
    addPlaceholder(cmdMenu, tr("De&lete tag..."), status);

    cmdMenu->addSeparator();
    addPlaceholder(cmdMenu, tr("C&herry pick..."), status);
    addPlaceholder(cmdMenu, tr("&Archive revision..."), status);
    addPlaceholder(cmdMenu, tr("Checko&ut revision..."), status);

    cmdMenu->addSeparator();
    addPlaceholder(cmdMenu, tr("&Bisect..."), status);

    cmdMenu->addSeparator();
    addPlaceholder(cmdMenu, tr("&Format patch..."), status);
    addPlaceholder(cmdMenu, tr("A&pply patch..."), status);

    // ---- Plugins ----
    auto* pluginsMenu = menuBar()->addMenu(tr("&Plugins"));
    addPlaceholder(pluginsMenu, tr("&Delete obsolete branches"), status);
    addPlaceholder(pluginsMenu, tr("&Find large files"), status);
    addPlaceholder(pluginsMenu, tr("&GitFlow"), status);
    addPlaceholder(pluginsMenu, tr("&Impact Graph"), status);
    addPlaceholder(pluginsMenu, tr("&Periodic background fetch"), status);
    addPlaceholder(pluginsMenu, tr("&Statistics"), status);
    pluginsMenu->addSeparator();
    addPlaceholder(pluginsMenu, tr("Plugin &Manager"), status);
    addPlaceholder(pluginsMenu, tr("Plugins &settings..."), status);

    // ---- Tools ----
    auto* toolsMenu = menuBar()->addMenu(tr("&Tools"));
    addPlaceholder(toolsMenu, tr("Git &bash"), status)
        ->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_G));
    addPlaceholder(toolsMenu, tr("Git&K"), status);
    toolsMenu->addSeparator();
    addPlaceholder(toolsMenu, tr("Git &command log"), status)
        ->setShortcut(QKeySequence(Qt::Key_F12));
    toolsMenu->addSeparator();
    toolsMenu->addAction(tr("&Settings..."),
                         QKeySequence::Preferences,
                         this, &MainWindow::showSettingsDialog);

    // ---- Help ----
    auto* helpMenu = menuBar()->addMenu(tr("&Help"));
    addPlaceholder(helpMenu, tr("&User manual"), status)
        ->setShortcut(QKeySequence::HelpContents);
    addPlaceholder(helpMenu, tr("&Changelog"), status);
    helpMenu->addSeparator();
    addPlaceholder(helpMenu, tr("&Report an issue"), status);
    addPlaceholder(helpMenu, tr("&Check for updates"), status);
    helpMenu->addSeparator();
    helpMenu->addAction(tr("&About GitBolt"), this, &MainWindow::showAbout);
}

// ---------------------------------------------------------------------------
// Toolbar
// ---------------------------------------------------------------------------
//
// Layout matches the GitExtensions browse window toolbar:
//
//   Refresh | Fetch Pull Push | Commit | Stash | Settings
//                                              | <stretch>
//                                              | Filter: [____]
//
// Several buttons are still wired to the placeholder helper that
// pops a status-bar message — we want them visible in the chrome
// so users can see what's coming.
// ---------------------------------------------------------------------------
void MainWindow::createToolBar()
{
    auto* toolbar = addToolBar(tr("Main"));
    toolbar->setMovable(false);
    toolbar->setIconSize(QSize(18, 18));
    toolbar->setObjectName(QStringLiteral("MainToolBar"));

    auto* status = statusBar();
    auto addToolbarPlaceholder = [&](const QString& label) {
        auto* a = toolbar->addAction(label);
        connect(a, &QAction::triggered, [label, status]() {
            if (status)
                status->showMessage(
                    tr("%1 — not yet implemented").arg(label), 3000);
        });
        return a;
    };

    // All repo-dependent actions reuse the QAction instances
    // already constructed in createMenuBar() — addAction(QAction*)
    // shares one action between the menu and the toolbar, so a
    // single setEnabled() call toggles both. This is why the menu
    // MUST be created before the toolbar (see the ordering in the
    // MainWindow constructor).
    toolbar->addAction(refreshAction_);

    toolbar->addSeparator();

    toolbar->addAction(fetchAction_);
    toolbar->addAction(pullAction_);
    toolbar->addAction(pushAction_);

    toolbar->addSeparator();

    toolbar->addAction(commitAction_);

    // Stash is still a placeholder — nothing to wire up yet.
    stashAction_    = addToolbarPlaceholder(tr("Stash"));
    stashAction_->setEnabled(false);

    // Real toolbar Settings button — opens the same modal dialog
    // as the Tools → Settings menu item. Not disabled with the
    // repo-dependent actions because the settings dialog works
    // whether a repo is open or not.
    settingsAction_ = toolbar->addAction(tr("Settings"));
    connect(settingsAction_, &QAction::triggered,
            this, &MainWindow::showSettingsDialog);

    // Right-aligned spacer so the filter sits at the far end like
    // GitExtensions' "Filter:" input.
    auto* spacer = new QWidget(toolbar);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbar->addWidget(spacer);

    auto* filterLabel = new QLabel(tr("Filter:"), toolbar);
    filterLabel->setContentsMargins(4, 0, 4, 0);
    toolbar->addWidget(filterLabel);

    filterInput_ = new QLineEdit(toolbar);
    filterInput_->setPlaceholderText(tr("Search commits…"));
    filterInput_->setClearButtonEnabled(true);
    filterInput_->setMaximumWidth(220);
    filterInput_->setEnabled(false);  // placeholder — wires up later
    filterInput_->setToolTip(
        tr("Quick filter for the revision grid (not yet implemented)"));
    toolbar->addWidget(filterInput_);
}

// ---------------------------------------------------------------------------
// Status bar
// ---------------------------------------------------------------------------
void MainWindow::createStatusBar()
{
    branchLabel_ = new QLabel(this);
    repoPathLabel_ = new QLabel(this);

    statusBar()->addWidget(branchLabel_);
    statusBar()->addWidget(repoPathLabel_, 1);
    statusBar()->showMessage(tr("Ready"));
}

// ---------------------------------------------------------------------------
// Signal/slot connections
// ---------------------------------------------------------------------------
void MainWindow::setupConnections()
{
    // --- GitService signals ---
    connect(gitService_, &services::GitService::repositoryOpened,
            this, &MainWindow::onRepositoryOpened);

    connect(gitService_, &services::GitService::logReady,
            this, &MainWindow::onLogReady);

    connect(gitService_, &services::GitService::branchesReady,
            this, &MainWindow::onBranchesReady);

    // Status-bar only surfacing for git op results. The Console
    // tab is an interactive terminal now — it's not the right
    // place to dump passive log messages.
    connect(gitService_, &services::GitService::operationFailed,
            this, [this](const QString& op, const QString& err) {
                statusBar()->showMessage(op + tr(" failed: ") + err, 5000);
            });

    connect(gitService_, &services::GitService::commitComplete,
            this, [this](bool success, const QString& message) {
                if (success)
                    statusBar()->showMessage(message, 3000);
                else
                    statusBar()->showMessage(tr("Commit failed: ") + message, 5000);
            });

    connect(gitService_, &services::GitService::repositoryChanged,
            this, [this]() {
                gitService_->refreshStatus();
                gitService_->refreshLog();
            });

    // --- CommitLogModel: request more commits (lazy loading) ---
    connect(commitLogModel_, &models::CommitLogModel::requestMoreCommits,
            this, [this](int offset, int /*count*/) {
                gitService_->refreshLog(offset);
            });

    // --- Dashboard: open-from-recent or clone/init ---
    connect(dashboardView_, &DashboardView::openRepositoryRequested,
            this, [this](const QString& path) {
                if (path.isEmpty())
                    openRepository();           // file picker dialog
                else
                    openRepositoryAtPath(path); // open the named path
            });
    connect(dashboardView_, &DashboardView::cloneRequested,
            this, &MainWindow::cloneRepository);

    // --- Branch tree: checkout (BranchTreeWidget lives inside RepositoryView) ---
    if (auto* branchTree = repoView_->branchTree()) {
        connect(branchTree, &widgets::BranchTreeWidget::checkoutRequested,
                gitService_, &services::GitService::checkoutBranch);
    }
}

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------
void MainWindow::openRepository()
{
    QString startDir;
    if (settingsService_) {
        const QStringList recent = settingsService_->recentRepositories();
        if (!recent.isEmpty()) {
            const QFileInfo fi(recent.first());
            startDir = fi.absolutePath();
        }
    }
    if (startDir.isEmpty())
        startDir = QDir::homePath();

    QString dir = QFileDialog::getExistingDirectory(this, tr("Open Repository"), startDir);
    if (!dir.isEmpty())
        openRepositoryAtPath(dir);
}

void MainWindow::openRepositoryAtPath(const QString& path)
{
    if (path.isEmpty())
        return;
    if (!gitService_->openRepository(path)) {
        QMessageBox::warning(this, tr("Error"),
            tr("Failed to open repository at %1").arg(path));
    }
}

// ---------------------------------------------------------------------------
// Clone — opens the modal CloneDialog. The dialog runs git_clone()
// on a worker thread and only returns Accepted once the clone has
// finished successfully, so by the time we read clonedPath() the
// new working directory is on disk and ready to be opened via the
// normal openRepositoryAtPath flow.
// ---------------------------------------------------------------------------
void MainWindow::cloneRepository()
{
    dialogs::CloneDialog dlg(this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    const QString path = dlg.clonedPath();
    if (path.isEmpty())
        return;
    openRepositoryAtPath(path);
}

void MainWindow::onRepositoryOpened(const QString& path)
{
    setWindowTitle(QStringLiteral("GitBolt - ") + path);
    centralStack_->setCurrentWidget(repoView_);
    settingsService_->addRecentRepository(path);
    updateRecentMenu();

    // Clear stale state from the previous repo before loading the new one.
    if (filterInput_)
        filterInput_->clear();
    if (repoView_) {
        repoView_->resetInspectorTabs();
        repoView_->setRepositoryPath(path);
    }

    // Repo switched — close any open CommitDialog; its staging state
    // belongs to the previous repo and would otherwise get stomped
    // by the next statusReady signal for the new one. Matches
    // GitExtensions behavior.
    if (commitDialog_ && commitDialog_->isVisible())
        commitDialog_->close();

    // Enable repo-dependent actions. Each of these lives as a
    // single QAction shared between the Repository menu and the
    // toolbar, so one setEnabled() call toggles BOTH entry points
    // in lockstep (see MainWindow.h for the rationale).
    if (refreshAction_) refreshAction_->setEnabled(true);
    if (fetchAction_)   fetchAction_->setEnabled(true);
    if (pullAction_)    pullAction_->setEnabled(true);
    if (pushAction_)    pushAction_->setEnabled(true);
    if (commitAction_)  commitAction_->setEnabled(true);
    if (filterInput_)   filterInput_->setEnabled(true);

    repoPathLabel_->setText(path);

    auto* repo = gitService_->repository();
    if (repo) {
        auto branchResult = repo->headBranchName();
        if (branchResult) {
            branchLabel_->setText(
                tr("Branch: %1").arg(QString::fromStdString(*branchResult)));
        } else {
            branchLabel_->setText(tr("HEAD (detached)"));
        }
    }

    statusBar()->showMessage(tr("Opened: %1").arg(path), 3000);
}

void MainWindow::onLogReady(std::vector<gitbolt::git::CommitData> commits, int offset)
{
    if (offset == 0) {
        commitLogModel_->setCommits(std::move(commits));
    } else {
        commitLogModel_->appendCommits(commits);
    }
}

void MainWindow::onBranchesReady(std::vector<gitbolt::git::BranchInfo> branches)
{
    // Forward branches to the repository view (which owns the
    // BranchTreeWidget inside its left pane).
    if (repoView_)
        repoView_->setBranches(std::move(branches));

    // Also update the branch label in the status bar.
    auto* repo = gitService_->repository();
    if (repo) {
        auto branchResult = repo->headBranchName();
        if (branchResult) {
            branchLabel_->setText(
                tr("Branch: %1").arg(QString::fromStdString(*branchResult)));
        }
    }
}

void MainWindow::showAbout()
{
    dialogs::AboutDialog dlg(this);
    dlg.exec();
}

// ---------------------------------------------------------------------------
// Settings dialog
// ---------------------------------------------------------------------------
//
// Constructed fresh on every open — the dialog snapshots current
// values in loadSettings() and writes through to SettingsService
// on Apply/OK, so there's no persistent state to cache. Stack
// allocation + exec() keeps the dialog modal and cleans up
// automatically when it closes.
void MainWindow::showSettingsDialog()
{
    dialogs::SettingsDialog dlg(settingsService_, themeService_, this);
    dlg.exec();
}

void MainWindow::setThemeService(conf::ThemeService* theme)
{
    themeService_ = theme;
}

void MainWindow::showCommitDialog()
{
    if (!gitService_ || !gitService_->isOpen())
        return;

    if (!commitDialog_) {
        commitDialog_ = new dialogs::CommitDialog(
            gitService_, settingsService_, this);
    }
    commitDialog_->show();
    commitDialog_->raise();
    commitDialog_->activateWindow();
}

void MainWindow::updateRecentMenu()
{
    if (!recentMenu_)
        return;

    recentMenu_->clear();

    QStringList recent = settingsService_->recentRepositories();
    if (recent.isEmpty()) {
        recentMenu_->addAction(tr("(No recent repositories)"))->setEnabled(false);
        return;
    }

    for (const QString& path : recent) {
        recentMenu_->addAction(path, this, [this, path]() {
            if (!gitService_->openRepository(path))
                QMessageBox::warning(this, tr("Error"),
                    tr("Failed to open repository at %1").arg(path));
        });
    }
}

} // namespace gitbolt::ui
