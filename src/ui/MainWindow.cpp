#include "ui/MainWindow.h"
#include "ui/RepositoryView.h"
#include "ui/DashboardView.h"
#include "dialogs/AboutDialog.h"
#include "dialogs/AdvancedFilterDialog.h"
#include "dialogs/BranchPickerDialog.h"
#include "dialogs/CherryPickDialog.h"
#include "dialogs/CloneDialog.h"
#include "dialogs/CommitDialog.h"
#include "dialogs/GitFlowDialog.h"
#include "dialogs/MaintenanceDialog.h"
#include "dialogs/RebaseDialog.h"
#include "dialogs/ReflogDialog.h"
#include "dialogs/RemotesDialog.h"
#include "dialogs/SettingsDialog.h"
#include "dialogs/StashDialog.h"
#include "dialogs/StashManageDialog.h"
#include "dialogs/TagDialog.h"
#include "dialogs/TextEditorDialog.h"
#include "dialogs/WorktreeDialog.h"
#include "git/GitProcessLog.h"
#include "models/CommitLogModel.h"
#include "services/GitService.h"
#include "conf/SettingsService.h"
#include "conf/ThemeService.h"
#include "widgets/BranchTreeWidget.h"
#include "widgets/ConsoleOutputWidget.h"
#include "widgets/MergeConflictWidget.h"
#include "widgets/RevisionGraphWidget.h"
#include "widgets/SubmoduleWidget.h"
#include "widgets/TerminalWidget.h"
#include "widgets/WorktreeWidget.h"
#include "models/CommitLogModel.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QPointer>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QIcon>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QHash>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QTimer>
#include <QComboBox>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QProcess>
#include <QTableWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QFile>
#include <QProcess>
#include <QStackedWidget>
#include <QTextBrowser>
#include <QStatusBar>
#include <QToolBar>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <functional>

namespace {

// Load a Material Symbols icon from the Qt resource system.
// SVGs live under :/icons/menu/<name>.svg and are already tinted
// with their semantic color (see gitbolt.qrc and resources/icons/menu/).
static QIcon menuIcon(const QString& name)
{
    return QIcon(QStringLiteral(":/icons/menu/") + name + QStringLiteral(".svg"));
}

// Surface a `Result<ProcessOutput>` to the user. Returns true on
// full success (spawn succeeded AND exit code == 0). Shows a
// QMessageBox::warning with the correct error detail otherwise —
// distinguishing "could not spawn git" (out.error()) from "git
// ran but returned non-zero" (out.value().stderrData), so the
// user sees merge conflicts, dirty-tree errors, ref-not-found,
// etc. rather than a silent no-op.
static bool handleProcessResult(QWidget* parent,
                                const QString& title,
                                const gitbolt::git::Result<gitbolt::git::ProcessOutput>& out)
{
    if (!out.ok()) {
        QMessageBox::warning(parent, title,
            QString::fromStdString(out.error().message()));
        return false;
    }
    if (!out.value().success()) {
        QString detail = QString::fromStdString(out.value().stderrData).trimmed();
        if (detail.isEmpty())
            detail = QString::fromStdString(out.value().stdoutData).trimmed();
        if (detail.isEmpty())
            detail = QObject::tr("git exited with code %1")
                        .arg(out.value().exitCode);
        QMessageBox::warning(parent, title, detail);
        return false;
    }
    return true;
}

} // namespace

namespace gitbolt::ui {

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("GitBolt"));

    // --- Core services ---
    gitService_ = new services::GitService(this);
    settingsService_ = new conf::SettingsService(this);

    // Apply startup window size. Two modes, controlled by the user
    // setting `restoreLastWindowSize()`:
    //   ON  (default) — restore the previous geometry on launch via
    //                    QMainWindow::restoreGeometry, so dragged
    //                    sizes persist. Falls through to the fixed-
    //                    size resize if no geometry has been saved
    //                    yet (first launch).
    //   OFF           — always resize to startupWindowWidth ×
    //                    startupWindowHeight, ignoring saved geom.
    bool sized = false;
    if (settingsService_->restoreLastWindowSize()) {
        const QByteArray geom = settingsService_->restoreWindowGeometry();
        if (!geom.isEmpty())
            sized = restoreGeometry(geom);
    }
    if (!sized) {
        resize(settingsService_->startupWindowWidth(),
               settingsService_->startupWindowHeight());
    }

    // Auto-prune Recent Repositories entries whose paths no longer
    // exist on disk OR no longer look like a git repo. This drops
    // stale entries from past clone-destination renames and moved
    // checkouts without the user having to right-click each one.
    // Done synchronously here because the list is bounded (default
    // cap 10) and a stat per entry is microseconds.
    {
        const QStringList recent =
            settingsService_->recentRepositories();
        for (const auto& path : recent) {
            const QFileInfo workdir(path);
            const QFileInfo gitDir(path + QStringLiteral("/.git"));
            const bool exists = workdir.exists() && workdir.isDir()
                && (gitDir.exists() ||
                    QFileInfo(path + QStringLiteral("/HEAD")).exists());
            if (!exists)
                settingsService_->removeRecentRepository(path);
        }
    }

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
    if (!settingsService_)
        return;

    // Save the main window geometry every time, regardless of the
    // user's `restoreLastWindowSize` preference — a saved record is
    // cheap and keeping it up-to-date means flipping the toggle
    // back ON later picks up the right size instead of an ancient
    // one.
    settingsService_->saveWindowGeometry(saveGeometry());

    if (!repoView_)
        return;
    settingsService_->saveSplitterState(
        QStringLiteral("repoSplitterH/v1"),
        repoView_->saveRepoSplitterH());
    settingsService_->saveSplitterState(
        QStringLiteral("repoSplitterV/v1"),
        repoView_->saveRepoSplitterV());
    // Bumped to v2 in lockstep with kDiffSplitterKey in
    // RepositoryView.cpp when the diff splitter's stretch ratio
    // was retuned. Writing v2 here pairs with reading v2 there.
    settingsService_->saveSplitterState(
        QStringLiteral("diffSplitter/v2"),
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
// Every entry below is wired to a real handler. We used to ship a
// few `addPlaceholder()` stubs (Show git notes, Show author avatar
// column, Impact Graph) that just flashed "not yet implemented"
// in the status bar — they were misleading and have been removed.
// When those features are actually built, add them back inline as
// real actions next to their siblings.
// ---------------------------------------------------------------------------

void MainWindow::createMenuBar()
{
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
        aboutAct->setObjectName(QStringLiteral("app.about"));
        aboutAct->setMenuRole(QAction::AboutRole);
        connect(aboutAct, &QAction::triggered, this, &MainWindow::showAbout);
    }
#endif

    // ---- File ----
    auto* fileMenu = menuBar()->addMenu(tr("&File"));

    auto* openAction = new QAction(menuIcon(QStringLiteral("open_repo")),
                                   tr("&Open Repository..."), this);
    openAction->setObjectName(QStringLiteral("file.open-repository"));
    openAction->setShortcut(QKeySequence::Open);
    connect(openAction, &QAction::triggered, this, &MainWindow::openRepository);
    fileMenu->addAction(openAction);

    auto* cloneAction = new QAction(menuIcon(QStringLiteral("clone_repo")),
                                    tr("&Clone Repository..."), this);
    cloneAction->setObjectName(QStringLiteral("file.clone-repository"));
    connect(cloneAction, &QAction::triggered, this, &MainWindow::cloneRepository);
    fileMenu->addAction(cloneAction);

    {
        // Pick a folder with QFileDialog, then Repository::init
        // creates `.git/` in it. If init succeeds we immediately
        // open the new repo so the user is dropped into the repo
        // view as if they'd cloned it.
        auto* a = new QAction(menuIcon(QStringLiteral("new_repo")),
                              tr("Create &New Repository..."), this);
        a->setObjectName(QStringLiteral("file.create-new-repository"));
        connect(a, &QAction::triggered, this, [this]() {
            const QString dir = QFileDialog::getExistingDirectory(
                this, tr("Choose a folder for the new repository"),
                QDir::homePath());
            if (dir.isEmpty()) return;
            auto res = git::Repository::init(dir.toStdString(), false);
            if (!res.ok()) {
                QMessageBox::warning(this, tr("Init Failed"),
                    tr("Could not initialize a repository at:\n%1\n\n%2")
                        .arg(dir, QString::fromStdString(res.error().message())));
                return;
            }
            openRepositoryAtPath(dir);
        });
        fileMenu->addAction(a);
    }

    recentMenu_ = fileMenu->addMenu(tr("Recent Repositories"));
    recentMenu_->setIcon(menuIcon(QStringLiteral("recent")));
    updateRecentMenu();

    fileMenu->addSeparator();
    // Home: swap the central stack back to the dashboard. Unlike
    // Close (on the Repository menu), this does NOT tear down the
    // open repo or gray out the repo-only menus — it's a pure view
    // switch, mirroring what the user asked for ("just takes you to
    // the home page"). The existing repo stays open; clicking the
    // same entry in Recent Repositories brings the repo view back.
    auto* homeAction = new QAction(menuIcon(QStringLiteral("recent")),
                                   tr("&Home"), this);
    homeAction->setObjectName(QStringLiteral("file.home"));
    connect(homeAction, &QAction::triggered, this, [this]() {
        if (centralStack_ && dashboardView_)
            centralStack_->setCurrentWidget(dashboardView_);
    });
    fileMenu->addAction(homeAction);

    fileMenu->addSeparator();
    auto* quitAction = new QAction(menuIcon(QStringLiteral("quit")),
                                   tr("&Quit"), this);
    quitAction->setObjectName(QStringLiteral("file.quit"));
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, qApp, &QApplication::quit);
    fileMenu->addAction(quitAction);

    // ---- Repository ----
    //
    // Refresh / Fetch / Pull / Push / Commit are created here as
    // shared QAction members so createToolBar() can add the SAME
    // instances to the toolbar — that way the disabled-without-repo
    // state is managed in exactly one place (onRepositoryOpened).
    repoMenu_ = menuBar()->addMenu(tr("&Repository"));
    auto* repoMenu = repoMenu_;

    refreshAction_ = new QAction(menuIcon(QStringLiteral("refresh")),
                                 tr("&Refresh"), this);
    // Explicit objectName (convention for shared / member actions —
    // stable across display-text changes, unlike the menu-path
    // fallback assignActionObjectNames() applies). See CONTRIBUTING.
    refreshAction_->setObjectName(QStringLiteral("act.refresh"));
    refreshAction_->setShortcut(QKeySequence::Refresh);
    refreshAction_->setEnabled(false);
    connect(refreshAction_, &QAction::triggered, this, [this]() {
        if (!gitService_ || !gitService_->isOpen())
            return;

        // Mirror what runRemoteOp does for fetch/pull/push: show an
        // inline indicator so the click is visible. Refresh fires
        // five async ops in parallel and there's no single "done"
        // signal to listen for, so we just flash the label for a
        // short window — enough to confirm the click registered.
        if (remoteOpClearTimer_ && remoteOpClearTimer_->isActive())
            remoteOpClearTimer_->stop();
        if (remoteOpLabel_) {
            remoteOpLabel_->setStyleSheet(QStringLiteral(
                "QLabel { color: palette(window-text); font-style: italic; }"));
            remoteOpLabel_->setText(tr("Refreshing…"));
        }
        statusBar()->showMessage(tr("Refreshing…"), 1500);

        gitService_->refreshStatus();
        gitService_->refreshLog();
        gitService_->refreshBranches();
        gitService_->refreshStashes();
        gitService_->refreshSubmodules();
        gitService_->refreshTags();

        if (!remoteOpClearTimer_) {
            remoteOpClearTimer_ = new QTimer(this);
            remoteOpClearTimer_->setSingleShot(true);
            connect(remoteOpClearTimer_, &QTimer::timeout, this, [this]() {
                if (remoteOpLabel_) {
                    remoteOpLabel_->clear();
                    remoteOpLabel_->setStyleSheet(QString());
                }
            });
        }
        remoteOpClearTimer_->start(1500);
    });
    repoMenu->addAction(refreshAction_);

    {
        // Reveal the repo's working directory in the OS file manager
        // (Finder on macOS, Explorer on Windows, default FM on
        // Linux). Matches GitExtensions' "File Explorer" entry.
        auto* a = new QAction(menuIcon(QStringLiteral("file_explorer")),
                              tr("File E&xplorer"), this);
        a->setObjectName(QStringLiteral("repository.file-explorer"));
        a->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O));
        connect(a, &QAction::triggered, this, [this]() {
            if (!gitService_ || !gitService_->isOpen())
                return;
            const QString path = QString::fromStdString(
                gitService_->withRepository(
                    [](git::Repository& r) { return r.workdir(); }));
            QDesktopServices::openUrl(QUrl::fromLocalFile(path));
        });
        repoMenu->addAction(a);
    }

    repoMenu->addSeparator();
    {
        // Remote repositories CRUD. RemotesDialog is modeless and
        // refreshes itself off Repository::remotes() after each
        // Add / EditUrl / Remove. EditUrl is a remove-then-add
        // round-trip since Repository doesn't expose a setUrl verb.
        auto* a = new QAction(menuIcon(QStringLiteral("remote")),
                              tr("Remote &repositories..."), this);
        a->setObjectName(QStringLiteral("repository.remote-repositories"));
        connect(a, &QAction::triggered, this, [this]() {
            if (!gitService_ || !gitService_->isOpen()) return;
            auto* dlg = new dialogs::RemotesDialog(this);
            dlg->setAttribute(Qt::WA_DeleteOnClose);

            auto refresh = [this, dlg]() {
                auto res = gitService_->withRepository(
                    [](git::Repository& r) { return r.remotes(); });
                if (res.ok()) dlg->setRemotes(res.value());
            };
            refresh();

            connect(dlg, &dialogs::RemotesDialog::addRequested,
                    this, [this, dlg, refresh](const QString& name,
                                               const QString& url) {
                auto res = gitService_->withRepository(
                    [&](git::Repository& r) {
                        return r.addRemote(name.toStdString(),
                                           url.toStdString());
                    });
                if (!res.ok()) {
                    QMessageBox::warning(dlg, tr("Add Remote Failed"),
                        QString::fromStdString(res.error().message()));
                }
                refresh();
            });
            connect(dlg, &dialogs::RemotesDialog::editUrlRequested,
                    this, [this, dlg, refresh](const QString& name,
                                               const QString& newUrl) {
                // Remove + re-add, both halves under ONE repo lock
                // so no background worker interleaves between them.
                // If the remove succeeds but the add fails we
                // surface the add error and the user is left without
                // that remote — they can re-add manually. Reasonable
                // trade-off vs. plumbing a setUrl path through
                // Repository.
                const auto outcome = gitService_->withRepository(
                    [&](git::Repository& repo)
                        -> std::pair<git::Result<void>,
                                     git::Result<void>> {
                        auto rm = repo.removeRemote(name.toStdString());
                        if (!rm.ok())
                            return {std::move(rm),
                                    git::Result<void>::success()};
                        auto add = repo.addRemote(
                            name.toStdString(), newUrl.toStdString());
                        return {std::move(rm), std::move(add)};
                    });
                const auto& rm = outcome.first;
                if (!rm.ok()) {
                    QMessageBox::warning(dlg, tr("Edit URL Failed"),
                        QString::fromStdString(rm.error().message()));
                    refresh();
                    return;
                }
                const auto& add = outcome.second;
                if (!add.ok()) {
                    QMessageBox::warning(dlg, tr("Edit URL Failed"),
                        tr("Removed remote but could not re-add "
                           "with new URL: %1").arg(
                            QString::fromStdString(
                                add.error().message())));
                }
                refresh();
            });
            connect(dlg, &dialogs::RemotesDialog::removeRequested,
                    this, [this, dlg, refresh](const QString& name) {
                const auto confirm = QMessageBox::question(
                    dlg, tr("Remove Remote"),
                    tr("Remove remote \"%1\"? Local branches "
                       "tracking this remote will lose their "
                       "upstream link.").arg(name),
                    QMessageBox::Yes | QMessageBox::Cancel,
                    QMessageBox::Cancel);
                if (confirm != QMessageBox::Yes) return;
                auto res = gitService_->withRepository(
                    [&](git::Repository& r) {
                        return r.removeRemote(name.toStdString());
                    });
                if (!res.ok()) {
                    QMessageBox::warning(dlg, tr("Remove Failed"),
                        QString::fromStdString(res.error().message()));
                }
                refresh();
            });

            dlg->show();
        });
        repoMenu->addAction(a);
    }

    repoMenu->addSeparator();
    {
        // SubmoduleWidget is normally an inspector tab — for the
        // menu entry we host it in a modeless QDialog and wire its
        // row-action signals to GitService. Left as "modeless" (open
        // via show()) rather than exec() so the user can interact
        // with the main window while browsing submodules.
        auto* a = new QAction(menuIcon(QStringLiteral("submodule")),
                              tr("Manage &submodules..."), this);
        a->setObjectName(QStringLiteral("repository.manage-submodules"));
        connect(a, &QAction::triggered, this, [this]() {
            if (!gitService_ || !gitService_->isOpen())
                return;
            auto* dlg = new QDialog(this);
            dlg->setAttribute(Qt::WA_DeleteOnClose);
            dlg->setWindowTitle(tr("Submodules"));
            dlg->resize(720, 420);
            auto* layout = new QVBoxLayout(dlg);
            layout->setContentsMargins(0, 0, 0, 0);
            auto* widget = new widgets::SubmoduleWidget(dlg);
            layout->addWidget(widget);

            // Prime the widget with the current submodule list and
            // keep it in sync as GitService re-emits after mutations.
            auto populate = [widget, this]() {
                auto res = gitService_->withRepository(
                    [](git::Repository& r) { return r.submodules(); });
                if (res.ok())
                    widget->setSubmodules(res.value());
                else
                    widget->clear();
            };
            populate();
            connect(gitService_, &services::GitService::submodulesReady,
                    widget, [widget](std::vector<git::SubmoduleInfo> s) {
                        widget->setSubmodules(std::move(s));
                    });

            // Row actions → GitService where a wrapper exists;
            // sync/deinit run through the git CLI (libgit2 has no
            // first-class equivalent) with the same error dialog
            // the bulk submodule menu items use.
            connect(widget, &widgets::SubmoduleWidget::initRequested,
                    this, [this](const QString& n) {
                        gitService_->submoduleInit(n);
                    });
            connect(widget, &widgets::SubmoduleWidget::updateRequested,
                    this, [this](const QString& n) {
                        gitService_->submoduleUpdate(n);
                    });
            connect(widget, &widgets::SubmoduleWidget::syncRequested,
                    this, [this](const QString& n) {
                        auto out = gitService_->process().run(
                            {"submodule", "sync", "--",
                             n.toStdString()});
                        handleProcessResult(
                            this, tr("Submodule Sync Failed"), out);
                        gitService_->refreshSubmodules();
                    });
            connect(widget, &widgets::SubmoduleWidget::deinitRequested,
                    this, [this, dlg](const QString& n) {
                        // Deinit empties the submodule's working
                        // tree — confirm before running, and pass
                        // -f so local modifications don't make git
                        // refuse after the user already said yes.
                        const auto answer = QMessageBox::question(
                            dlg, tr("Deinit Submodule"),
                            tr("Deinit '%1'?\n\nThis clears the "
                               "submodule's working tree (its "
                               "content can be restored later with "
                               "Init + Update).").arg(n));
                        if (answer != QMessageBox::Yes)
                            return;
                        auto out = gitService_->process().run(
                            {"submodule", "deinit", "-f", "--",
                             n.toStdString()});
                        handleProcessResult(
                            this, tr("Submodule Deinit Failed"), out);
                        gitService_->refreshSubmodules();
                    });
            connect(widget, &widgets::SubmoduleWidget::openRequested,
                    this, [](const QString& p) {
                        QDesktopServices::openUrl(QUrl::fromLocalFile(p));
                    });

            dlg->show();
        });
        repoMenu->addAction(a);
    }
    {
        // `git submodule update --init --recursive` in one shot —
        // the common case of "update EVERY submodule and their
        // nested children." No confirm; this is a read-heavy
        // operation (fetches + checkouts) but not destructive.
        auto* a = new QAction(menuIcon(QStringLiteral("submodule_update")),
                              tr("&Update all submodules"), this);
        a->setObjectName(QStringLiteral("repository.update-all-submodules"));
        connect(a, &QAction::triggered, this, [this]() {
            if (!gitService_ || !gitService_->isOpen()) return;
            auto out = gitService_->process().run(
                {"submodule", "update", "--init", "--recursive"},
                /*timeoutMs=*/120000);
            handleProcessResult(this, tr("Submodule Update Failed"), out);
            gitService_->refreshSubmodules();
        });
        repoMenu->addAction(a);
    }
    {
        // `git submodule sync --recursive` — propagates URL changes
        // from `.gitmodules` into each submodule's local `.git/config`.
        auto* a = new QAction(menuIcon(QStringLiteral("submodule_sync")),
                              tr("S&ynchronize all submodules"), this);
        a->setObjectName(QStringLiteral("repository.synchronize-all-submodules"));
        connect(a, &QAction::triggered, this, [this]() {
            if (!gitService_ || !gitService_->isOpen()) return;
            auto out = gitService_->process().run(
                {"submodule", "sync", "--recursive"});
            handleProcessResult(this, tr("Submodule Sync Failed"), out);
            gitService_->refreshSubmodules();
        });
        repoMenu->addAction(a);
    }

    repoMenu->addSeparator();
    {
        // Manage worktrees — modeless manager dialog hosting
        // WorktreeWidget (list + Add / Remove / Lock / Unlock /
        // Open), same hosting pattern as Manage Submodules. The
        // Add button opens the existing WorktreeDialog form.
        // Previously this menu item went STRAIGHT to the add form:
        // worktrees could be created but never listed, removed, or
        // locked from the UI.
        auto* a = new QAction(menuIcon(QStringLiteral("worktrees")),
                              tr("Manage &worktrees..."), this);
        a->setObjectName(QStringLiteral("repository.manage-worktrees"));
        a->setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_W));
        connect(a, &QAction::triggered, this, [this]() {
            if (!gitService_ || !gitService_->isOpen())
                return;

            auto* dlg = new QDialog(this);
            dlg->setAttribute(Qt::WA_DeleteOnClose);
            dlg->setWindowTitle(tr("Worktrees"));
            dlg->setObjectName(QStringLiteral("dlg.worktrees"));
            conf::SettingsService::applyConfiguredSize(dlg, "worktrees");

            auto* layout = new QVBoxLayout(dlg);
            layout->setContentsMargins(0, 0, 0, 0);
            auto* widget = new widgets::WorktreeWidget(dlg);
            layout->addWidget(widget);

            connect(gitService_,
                    &services::GitService::worktreesReady,
                    widget,
                    [widget](std::vector<git::WorktreeInfo> w) {
                        widget->setWorktrees(std::move(w));
                    });
            gitService_->refreshWorktrees();

            connect(widget, &widgets::WorktreeWidget::addRequested,
                    this, [this]() {
                dialogs::WorktreeDialog addDlg(this);
                QStringList branchNames;
                if (auto res = gitService_->withRepository(
                        [](git::Repository& r) {
                            return r.branches(git::BranchType::Local);
                        }); res.ok()) {
                    for (const auto& b : res.value())
                        branchNames << QString::fromStdString(b.name);
                }
                addDlg.setBranches(branchNames);
                if (addDlg.exec() == QDialog::Accepted) {
                    gitService_->addWorktree(addDlg.worktreeName(),
                                             addDlg.worktreePath(),
                                             addDlg.branch());
                }
            });
            connect(widget, &widgets::WorktreeWidget::removeRequested,
                    this, [this, dlg](const QString& name) {
                const auto answer = QMessageBox::question(
                    dlg, tr("Remove Worktree"),
                    tr("Remove worktree '%1'?\n\nIts working "
                       "directory is deleted from disk. Commits "
                       "made there stay in the repository, but "
                       "uncommitted changes are lost.").arg(name));
                if (answer == QMessageBox::Yes)
                    gitService_->removeWorktree(name);
            });
            connect(widget, &widgets::WorktreeWidget::lockRequested,
                    gitService_, &services::GitService::lockWorktree);
            connect(widget, &widgets::WorktreeWidget::unlockRequested,
                    gitService_, &services::GitService::unlockWorktree);
            connect(widget, &widgets::WorktreeWidget::openRequested,
                    this, [](const QString& p) {
                QDesktopServices::openUrl(QUrl::fromLocalFile(p));
            });

            dlg->show();
        });
        repoMenu->addAction(a);
    }

    repoMenu->addSeparator();
    {
        // Shared lambda: open a TextEditorDialog rooted at one of the
        // four config files git uses for path-pattern rules. The four
        // menu items below all delegate to this; the only thing that
        // varies is the filename relative to the workdir (or .git/
        // for info/exclude). Rather than duplicate four dialog calls
        // we capture this once and pass the relative path. Creates
        // the file with empty content if it doesn't exist yet so the
        // user can start a fresh .gitignore from the menu.
        auto editConfigFile = [this](const QString& relativePath,
                                     const QString& title) {
            if (!gitService_ || !gitService_->isOpen()) return;
            const QString workdir = QString::fromStdString(
                gitService_->withRepository(
                    [](git::Repository& r) { return r.workdir(); }));
            QString fullPath = workdir;
            if (!fullPath.endsWith('/')) fullPath += '/';
            fullPath += relativePath;

            QFile f(fullPath);
            QString original;
            if (f.exists()) {
                if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
                    QMessageBox::warning(this, title,
                        tr("Could not open %1 for reading.").arg(fullPath));
                    return;
                }
                original = QString::fromUtf8(f.readAll());
                f.close();
            }

            dialogs::TextEditorDialog dlg(this);
            dlg.setTitle(title);
            dlg.setLabel(fullPath);
            dlg.setContents(original);
            if (dlg.exec() != QDialog::Accepted) return;

            const QString updated = dlg.contents();
            if (updated == original) return;  // nothing changed

            // Ensure the parent directory exists — .git/info/ usually
            // does, but a brand-new repo may not yet have it. Saving
            // a fresh .gitignore at workdir always works.
            QDir().mkpath(QFileInfo(fullPath).absolutePath());
            if (!f.open(QIODevice::WriteOnly | QIODevice::Text |
                        QIODevice::Truncate)) {
                QMessageBox::warning(this, title,
                    tr("Could not write %1.").arg(fullPath));
                return;
            }
            f.write(updated.toUtf8());
            f.close();
            // Refresh status — the file system watcher will see the
            // change too, but a manual refresh keeps the inspector
            // in sync immediately.
            gitService_->refreshStatus();
        };

        auto* aGI = new QAction(menuIcon(QStringLiteral("edit")),
                                tr("Edit .&gitignore"), this);
        aGI->setObjectName(QStringLiteral("repository.edit-gitignore"));
        connect(aGI, &QAction::triggered, this, [editConfigFile]() {
            editConfigFile(QStringLiteral(".gitignore"),
                           tr("Edit .gitignore"));
        });
        repoMenu->addAction(aGI);

        auto* aEx = new QAction(menuIcon(QStringLiteral("edit")),
                                tr("Edit .git/&info/exclude"), this);
        aEx->setObjectName(QStringLiteral("repository.edit-git-info-exclude"));
        connect(aEx, &QAction::triggered, this, [editConfigFile]() {
            editConfigFile(QStringLiteral(".git/info/exclude"),
                           tr("Edit .git/info/exclude"));
        });
        repoMenu->addAction(aEx);

        auto* aAttr = new QAction(menuIcon(QStringLiteral("edit")),
                                  tr("Edit .git&attributes"), this);
        aAttr->setObjectName(QStringLiteral("repository.edit-gitattributes"));
        connect(aAttr, &QAction::triggered, this, [editConfigFile]() {
            editConfigFile(QStringLiteral(".gitattributes"),
                           tr("Edit .gitattributes"));
        });
        repoMenu->addAction(aAttr);

        auto* aMM = new QAction(menuIcon(QStringLiteral("edit")),
                                tr("Edit .&mailmap"), this);
        aMM->setObjectName(QStringLiteral("repository.edit-mailmap"));
        connect(aMM, &QAction::triggered, this, [editConfigFile]() {
            editConfigFile(QStringLiteral(".mailmap"),
                           tr("Edit .mailmap"));
        });
        repoMenu->addAction(aMM);
    }
    {
        // Sparse Working Copy — submenu with the three operations
        // most users actually want:
        //   • Init (cone mode) — turns on sparse-checkout
        //   • Set / edit patterns — opens .git/info/sparse-checkout
        //     in the text editor dialog (just like .gitignore)
        //   • Disable — `git sparse-checkout disable`
        auto* sub = repoMenu->addMenu(menuIcon(QStringLiteral("sparse")),
                                      tr("Sparse &Working Copy"));

        auto* initA = new QAction(tr("&Initialize (cone mode)"), this);
        initA->setObjectName(QStringLiteral("repository.sparse-working-copy.initialize-cone-mode"));
        connect(initA, &QAction::triggered, this, [this]() {
            if (!gitService_ || !gitService_->isOpen()) return;
            const auto out = gitService_->process().run(
                {"sparse-checkout", "init", "--cone"});
            handleProcessResult(this, tr("Sparse Init Failed"), out);
            gitService_->refreshStatus();
        });
        sub->addAction(initA);

        auto* setA = new QAction(tr("&Edit patterns..."), this);
        setA->setObjectName(QStringLiteral("repository.sparse-working-copy.edit-patterns"));
        connect(setA, &QAction::triggered, this, [this]() {
            if (!gitService_ || !gitService_->isOpen()) return;
            // The sparse-checkout file lives at
            //   .git/info/sparse-checkout
            // and accepts gitignore-style patterns (one per line).
            // Reuse TextEditorDialog and write back via
            // `git sparse-checkout set --stdin` so cone mode is
            // honored — writing the file directly works too but
            // skips git's own validation.
            const QString workdir = QString::fromStdString(
                gitService_->withRepository(
                    [](git::Repository& r) { return r.workdir(); }));
            const QString filePath = workdir +
                QStringLiteral("/.git/info/sparse-checkout");
            QFile f(filePath);
            QString original;
            if (f.exists() && f.open(QIODevice::ReadOnly | QIODevice::Text)) {
                original = QString::fromUtf8(f.readAll());
                f.close();
            }

            dialogs::TextEditorDialog dlg(this);
            dlg.setTitle(tr("Sparse Checkout Patterns"));
            dlg.setLabel(tr("One pattern per line, gitignore-style "
                            "(prefix with !  to exclude). Cone mode: "
                            "directory paths only."));
            dlg.setContents(original);
            if (dlg.exec() != QDialog::Accepted) return;
            const QString updated = dlg.contents();
            if (updated == original) return;

            QProcess proc;
            proc.setWorkingDirectory(workdir);
            proc.start("git",
                {"sparse-checkout", "set", "--stdin"});
            if (!proc.waitForStarted(5000)) {
                QMessageBox::warning(this, tr("Sparse Set Failed"),
                    tr("Could not start `git sparse-checkout`."));
                return;
            }
            proc.write(updated.toUtf8());
            proc.closeWriteChannel();
            if (!proc.waitForFinished(60000) || proc.exitCode() != 0) {
                QMessageBox::warning(this, tr("Sparse Set Failed"),
                    QString::fromUtf8(proc.readAllStandardError()));
                return;
            }
            gitService_->refreshStatus();
        });
        sub->addAction(setA);

        auto* disableA = new QAction(tr("&Disable"), this);
        disableA->setObjectName(QStringLiteral("repository.sparse-working-copy.disable"));
        connect(disableA, &QAction::triggered, this, [this]() {
            if (!gitService_ || !gitService_->isOpen()) return;
            const auto confirm = QMessageBox::question(this,
                tr("Disable Sparse Checkout"),
                tr("Restore the full working tree? All ignored "
                   "files come back."),
                QMessageBox::Yes | QMessageBox::Cancel,
                QMessageBox::Cancel);
            if (confirm != QMessageBox::Yes) return;
            const auto out = gitService_->process().run(
                {"sparse-checkout", "disable"});
            handleProcessResult(this, tr("Sparse Disable Failed"), out);
            gitService_->refreshStatus();
        });
        sub->addAction(disableA);
    }

    repoMenu->addSeparator();
    {
        auto* a = new QAction(menuIcon(QStringLiteral("maintenance")),
                              tr("Git mai&ntenance"), this);
        a->setObjectName(QStringLiteral("repository.git-maintenance"));
        connect(a, &QAction::triggered, this, [this]() {
            if (!gitService_ || !gitService_->isOpen())
                return;
            dialogs::MaintenanceDialog dlg(gitService_, this);
            dlg.exec();
        });
        repoMenu->addAction(a);
    }

    // "Repository settings" used to live here but was removed:
    // Git Extensions doesn't have a dedicated per-repo settings
    // dialog either — settings are split by storage location
    // (.git/config vs GitExtensions.settings) inside one unified
    // Tools → Settings. Keeping two entry points named "Settings"
    // (global and repo-scoped) was confusing.

    repoMenu->addSeparator();
    auto* closeAction = new QAction(menuIcon(QStringLiteral("close")),
                                    tr("&Close"), this);
    closeAction->setObjectName(QStringLiteral("repository.close"));
    closeAction->setShortcut(QKeySequence::Close);
    connect(closeAction, &QAction::triggered, this, [this]() {
        centralStack_->setCurrentWidget(dashboardView_);
        setWindowTitle(QStringLiteral("GitBolt"));
        // Back to the home screen — gray out every item in the
        // repo-only menus again, mirroring the initial state set at
        // the end of createMenuBar().
        setRepoOnlyMenusEnabled(false);
        // Also disable the repo-dependent toolbar/menu actions
        // shared between the Repository menu and the toolbar.
        setRepoActionsEnabled(false);
        if (branchCombo_) {
            branchCombo_->clear();
            branchCombo_->setProperty("currentBranch", QString());
        }
        // Hide the filter pair and branch combo alongside disabling
        // the rest of the repo-only toolbar entries — back to the
        // home look.
        if (filterLabelAction_) filterLabelAction_->setVisible(false);
        if (filterInputAction_) filterInputAction_->setVisible(false);
        if (branchLabelAction_) branchLabelAction_->setVisible(false);
        if (branchComboAction_) branchComboAction_->setVisible(false);
        if (remoteOpLabelAction_) remoteOpLabelAction_->setVisible(false);
        if (remoteOpLabel_) remoteOpLabel_->clear();
        // Clear the dynamic Commit (N) suffix back to plain "Commit"
        // — no count is meaningful when no repo is open.
        if (commitAction_)
            commitAction_->setText(tr("Co&mmit..."));
    });
    repoMenu->addAction(closeAction);

    // ---- Navigate ----
    navMenu_ = menuBar()->addMenu(tr("&Navigate"));
    auto* navMenu = navMenu_;
    {
        // Go to current revision → resolve HEAD and select it in
        // the graph. Surfaces errors to the status bar so the
        // user knows why nothing happened (unborn HEAD, or HEAD
        // isn't in the loaded commit window).
        auto* a = new QAction(menuIcon(QStringLiteral("current_rev")),
                              tr("Go to &current revision"), this);
        a->setObjectName(QStringLiteral("navigate.go-to-current-revision"));
        a->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C));
        connect(a, &QAction::triggered, this, [this]() {
            if (!gitService_ || !gitService_->isOpen() || !repoView_)
                return;
            auto* graph = repoView_->revisionGraph();
            if (!graph) return;
            auto headRes = gitService_->withRepository(
                [](git::Repository& r) { return r.head(); });
            if (!headRes.ok()) {
                statusBar()->showMessage(
                    tr("Could not resolve HEAD: %1").arg(
                        QString::fromStdString(headRes.error().message())),
                    4000);
                return;
            }
            if (!graph->selectCommit(QString::fromStdString(
                        headRes.value().toHex()))) {
                statusBar()->showMessage(
                    tr("HEAD isn't in the currently-loaded commit window."),
                    4000);
            }
        });
        navMenu->addAction(a);
    }
    {
        // Go to commit → prompt for a revision spec and select that
        // row in the graph. Accepts anything libgit2's revparse
        // resolves: full / short SHAs, branch names, tag names,
        // HEAD~3, refs/heads/foo, etc. The graph still requires the
        // full SHA to find the row, so we resolve to ObjectId first.
        auto* a = new QAction(menuIcon(QStringLiteral("go_to_commit")),
                              tr("Go to c&ommit..."), this);
        a->setObjectName(QStringLiteral("navigate.go-to-commit"));
        a->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_G));
        connect(a, &QAction::triggered, this, [this]() {
            if (!gitService_ || !gitService_->isOpen() || !repoView_)
                return;
            auto* graph = repoView_->revisionGraph();
            if (!graph) return;
            bool ok = false;
            const QString spec = QInputDialog::getText(
                this, tr("Go to Commit"),
                tr("Revision (SHA, branch, tag, HEAD~3, etc.):"),
                QLineEdit::Normal, QString(), &ok);
            if (!ok) return;
            const QString trimmed = spec.trimmed();
            if (trimmed.isEmpty()) return;
            auto resolved = gitService_->withRepository(
                [&](git::Repository& r) {
                    return r.resolveRef(trimmed.toStdString());
                });
            if (!resolved.ok()) {
                QMessageBox::information(this, tr("Not Found"),
                    tr("Couldn't resolve \"%1\": %2").arg(
                        trimmed,
                        QString::fromStdString(
                            resolved.error().message())));
                return;
            }
            const QString fullHex = QString::fromStdString(
                resolved.value().toHex());
            if (!graph->selectCommit(fullHex)) {
                QMessageBox::information(this, tr("Not Found"),
                    tr("That commit (%1) isn't in the currently-"
                       "loaded commit window.").arg(fullHex.left(8)));
            }
        });
        navMenu->addAction(a);
    }

    navMenu->addSeparator();
    {
        // Cmd+N → child commit. "Newer" direction in the graph
        // (lower row index). Falls back to a status-bar message if
        // no child is in the loaded commit window.
        auto* a = new QAction(menuIcon(QStringLiteral("child_commit")),
                              tr("Go to &child commit"), this);
        a->setObjectName(QStringLiteral("navigate.go-to-child-commit"));
        a->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_N));
        connect(a, &QAction::triggered, this, [this]() {
            if (!repoView_) return;
            auto* graph = repoView_->revisionGraph();
            if (!graph) return;
            if (!graph->selectFirstChild()) {
                statusBar()->showMessage(
                    tr("No child commit in the loaded window."), 4000);
            }
        });
        navMenu->addAction(a);
    }
    {
        // Cmd+P → first-parent commit. "Older" direction. For merge
        // commits this follows the mainline (where the user was
        // before merging) which matches `git log --first-parent`.
        auto* a = new QAction(menuIcon(QStringLiteral("parent_commit")),
                              tr("Go to &parent commit"), this);
        a->setObjectName(QStringLiteral("navigate.go-to-parent-commit"));
        a->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_P));
        connect(a, &QAction::triggered, this, [this]() {
            if (!repoView_) return;
            auto* graph = repoView_->revisionGraph();
            if (!graph) return;
            if (!graph->selectFirstParent()) {
                statusBar()->showMessage(
                    tr("No parent commit (root, or parent not loaded)."),
                    4000);
            }
        });
        navMenu->addAction(a);
    }
    {
        // Go to first parent — same as Cmd+P (which is "Go to
        // parent" today), but exposed as an explicit menu entry
        // for users who prefer mouse navigation. For non-merge
        // commits this matches the regular parent navigation.
        auto* a = new QAction(menuIcon(QStringLiteral("first_parent")),
                              tr("Go to &first parent commit"), this);
        a->setObjectName(QStringLiteral("navigate.go-to-first-parent-commit"));
        connect(a, &QAction::triggered, this, [this]() {
            if (!repoView_) return;
            if (auto* g = repoView_->revisionGraph()) {
                if (!g->selectFirstParent())
                    statusBar()->showMessage(
                        tr("No first parent (root, or not loaded)."),
                        4000);
            }
        });
        navMenu->addAction(a);
    }
    {
        // Go to last parent — for merge commits, the branch that
        // was merged in (not the mainline). Lets the user follow
        // the merged-in side of a merge instead of the integration
        // branch. Same as first parent for non-merge commits.
        auto* a = new QAction(menuIcon(QStringLiteral("last_parent")),
                              tr("Go to &last parent commit"), this);
        a->setObjectName(QStringLiteral("navigate.go-to-last-parent-commit"));
        connect(a, &QAction::triggered, this, [this]() {
            if (!repoView_) return;
            if (auto* g = repoView_->revisionGraph()) {
                if (!g->selectLastParent())
                    statusBar()->showMessage(
                        tr("No last parent (root, or not loaded)."),
                        4000);
            }
        });
        navMenu->addAction(a);
    }

    navMenu->addSeparator();
    {
        // Cmd+[ → pop the back stack and select that commit.
        // Pushes the current commit onto the forward stack so a
        // matching Cmd+] takes the user back where they were.
        // Used to be Alt+Left but on macOS that's "previous word"
        // in any QLineEdit, so the filter field swallows it.
        // Cmd+[ / Cmd+] is the macOS browser convention and never
        // collides with text-cursor navigation.
        auto* a = new QAction(menuIcon(QStringLiteral("back")),
                              tr("Navigate &backward"), this);
        a->setObjectName(QStringLiteral("navigate.navigate-backward"));
        a->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_BracketLeft));
        connect(a, &QAction::triggered, this, [this]() {
            if (backHistory_.isEmpty() || !repoView_) {
                statusBar()->showMessage(
                    tr("No earlier commit in history."), 4000);
                return;
            }
            auto* graph = repoView_->revisionGraph();
            if (!graph) return;
            const QString prev = backHistory_.takeLast();
            if (!currentNavCommit_.isEmpty())
                forwardHistory_.push_back(currentNavCommit_);
            suppressHistoryPush_ = true;
            if (!graph->selectCommit(prev)) {
                // Out-of-window — drop it from the stack and tell
                // the user instead of leaving things in a half-state.
                suppressHistoryPush_ = false;
                statusBar()->showMessage(
                    tr("Earlier commit %1 isn't in the loaded "
                       "window.").arg(prev.left(8)), 4000);
            }
        });
        navMenu->addAction(a);
    }
    {
        // Cmd+] → mirror image of Backward. Forward is empty
        // until the user presses Back at least once. Same shortcut
        // collision rationale as Backward — Alt+Right is "next
        // word" on macOS, so we use the browser convention.
        auto* a = new QAction(menuIcon(QStringLiteral("forward")),
                              tr("Navigate f&orward"), this);
        a->setObjectName(QStringLiteral("navigate.navigate-forward"));
        a->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_BracketRight));
        connect(a, &QAction::triggered, this, [this]() {
            if (forwardHistory_.isEmpty() || !repoView_) {
                statusBar()->showMessage(
                    tr("No later commit in history."), 4000);
                return;
            }
            auto* graph = repoView_->revisionGraph();
            if (!graph) return;
            const QString next = forwardHistory_.takeLast();
            if (!currentNavCommit_.isEmpty())
                backHistory_.push_back(currentNavCommit_);
            suppressHistoryPush_ = true;
            if (!graph->selectCommit(next)) {
                suppressHistoryPush_ = false;
                statusBar()->showMessage(
                    tr("Later commit %1 isn't in the loaded "
                       "window.").arg(next.left(8)), 4000);
            }
        });
        navMenu->addAction(a);
    }

    navMenu->addSeparator();
    {
        // Quick search → focus the toolbar Filter input. Cmd+F is
        // the macOS "find" convention; keeps the user's hands on
        // the keyboard. The filter input itself is wired to the
        // commit log via the existing filter signal in
        // RepositoryView.
        auto* a = new QAction(menuIcon(QStringLiteral("go_to_commit")),
                              tr("Quick &search"), this);
        a->setObjectName(QStringLiteral("navigate.quick-search"));
        a->setShortcut(QKeySequence::Find);
        connect(a, &QAction::triggered, this, [this]() {
            if (filterInput_ && filterInput_->isEnabled()) {
                filterInput_->setFocus(Qt::ShortcutFocusReason);
                filterInput_->selectAll();
            }
        });
        navMenu->addAction(a);
    }
    {
        // Quick search previous → previous visible row in the
        // proxy-filtered table. Alt+Up doesn't collide with text
        // navigation in the way Alt+Left does (Alt+Up isn't a
        // standard text shortcut on macOS), so we keep the
        // original mapping.
        auto* a = new QAction(menuIcon(QStringLiteral("parent_commit")),
                              tr("Quick search pre&vious"), this);
        a->setObjectName(QStringLiteral("navigate.quick-search-previous"));
        a->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Up));
        connect(a, &QAction::triggered, this, [this]() {
            if (repoView_ && repoView_->revisionGraph()) {
                if (!repoView_->revisionGraph()->selectPreviousMatch())
                    statusBar()->showMessage(
                        tr("No previous match."), 3000);
            }
        });
        navMenu->addAction(a);
    }
    {
        auto* a = new QAction(menuIcon(QStringLiteral("child_commit")),
                              tr("Quick search ne&xt"), this);
        a->setObjectName(QStringLiteral("navigate.quick-search-next"));
        a->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Down));
        connect(a, &QAction::triggered, this, [this]() {
            if (repoView_ && repoView_->revisionGraph()) {
                if (!repoView_->revisionGraph()->selectNextMatch())
                    statusBar()->showMessage(
                        tr("No next match."), 3000);
            }
        });
        navMenu->addAction(a);
    }

    // ---- View ----
    viewMenu_ = menuBar()->addMenu(tr("&View"));
    auto* viewMenu = viewMenu_;

    //
    // -- Branches section --
    //
    // Three mutually-exclusive log-scope actions:
    //   * Show all branches       -> walk every local branch tip
    //   * Show filtered branches  -> walk only the user-picked subset
    //   * Show current branch     -> walk just HEAD (default)
    // Driven by GitService::LogScope. Selecting "filtered" opens a
    // BranchPickerDialog; on Cancel we revert to the previously
    // active scope so the radio doesn't get stuck on "filtered"
    // without an actual selection behind it.
    {
        auto* group = new QActionGroup(this);
        group->setExclusive(true);

        auto* showAll = new QAction(menuIcon(QStringLiteral("visibility")),
                                    tr("Show &all branches"), this);
        showAll->setObjectName(QStringLiteral("view.show-all-branches"));
        showAll->setCheckable(true);
        showAll->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_A));
        group->addAction(showAll);
        viewMenu->addAction(showAll);

        auto* showFiltered = new QAction(menuIcon(QStringLiteral("filter")),
                                         tr("Show &filtered branches..."), this);
        showFiltered->setObjectName(QStringLiteral("view.show-filtered-branches"));
        showFiltered->setCheckable(true);
        showFiltered->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_T));
        group->addAction(showFiltered);
        viewMenu->addAction(showFiltered);

        auto* showHead = new QAction(menuIcon(QStringLiteral("visibility")),
                                     tr("Show c&urrent branch only"), this);
        showHead->setObjectName(QStringLiteral("view.show-current-branch-only"));
        showHead->setCheckable(true);
        showHead->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_U));
        group->addAction(showHead);
        viewMenu->addAction(showHead);

        // Restore from settings. "filtered" requires a non-empty
        // saved branch list — if the list got wiped (e.g. all
        // branches deleted), fall back to "head" so we don't show
        // an empty log on startup.
        const QString savedScope = settingsService_
            ? settingsService_->value("view/logScope", "head").toString()
            : QStringLiteral("head");
        const QStringList savedSelected = settingsService_
            ? settingsService_->value("view/selectedBranches",
                                      QStringList{}).toStringList()
            : QStringList{};
        if (savedScope == QLatin1String("all")) {
            showAll->setChecked(true);
            if (gitService_)
                gitService_->setLogScope(
                    services::GitService::LogScope::AllLocalBranches);
        } else if (savedScope == QLatin1String("filtered") &&
                   !savedSelected.isEmpty()) {
            showFiltered->setChecked(true);
            if (gitService_) {
                gitService_->setSelectedBranches(savedSelected);
                gitService_->setLogScope(
                    services::GitService::LogScope::SelectedBranches);
            }
        } else {
            showHead->setChecked(true);
        }

        connect(showAll, &QAction::triggered, this, [this]() {
            if (gitService_)
                gitService_->setLogScope(
                    services::GitService::LogScope::AllLocalBranches);
            if (settingsService_)
                settingsService_->setValue("view/logScope",
                                           QStringLiteral("all"));
        });
        connect(showHead, &QAction::triggered, this, [this]() {
            if (gitService_)
                gitService_->setLogScope(
                    services::GitService::LogScope::Head);
            if (settingsService_)
                settingsService_->setValue("view/logScope",
                                           QStringLiteral("head"));
        });
        connect(showFiltered, &QAction::triggered, this,
                [this, showAll, showHead, showFiltered]() {
            // Capture which action WOULD have been checked before
            // the group flipped to showFiltered. The group has
            // already updated by the time we get here, so we
            // can't ask the group; we infer from the live
            // GitService scope (it hasn't changed yet).
            const auto previousScope = gitService_
                ? gitService_->logScope()
                : services::GitService::LogScope::Head;

            // Build the branch list. If GitService isn't open yet
            // there's nothing to filter against; bail and revert.
            if (!gitService_ || !gitService_->isOpen()) {
                showHead->setChecked(true);
                return;
            }
            auto branchesRes = gitService_->withRepository(
                [](git::Repository& r) {
                    return r.branches(git::BranchType::Local);
                });
            if (!branchesRes.ok()) {
                showHead->setChecked(true);
                return;
            }
            QStringList all;
            for (const auto& b : branchesRes.value())
                all << QString::fromStdString(b.name);

            dialogs::BranchPickerDialog dlg(this);
            dlg.setBranches(all, gitService_->selectedBranches());
            if (dlg.exec() != QDialog::Accepted) {
                // User cancelled — restore previous radio + scope.
                switch (previousScope) {
                case services::GitService::LogScope::AllLocalBranches:
                    showAll->setChecked(true);
                    break;
                case services::GitService::LogScope::SelectedBranches:
                    showFiltered->setChecked(true);
                    break;
                case services::GitService::LogScope::Head:
                default:
                    showHead->setChecked(true);
                    break;
                }
                return;
            }
            const QStringList picked = dlg.selectedBranches();
            if (picked.isEmpty()) {
                // Empty selection makes "filtered" meaningless;
                // treat as Cancel-equivalent and revert.
                switch (previousScope) {
                case services::GitService::LogScope::AllLocalBranches:
                    showAll->setChecked(true);
                    break;
                case services::GitService::LogScope::SelectedBranches:
                    showFiltered->setChecked(true);
                    break;
                case services::GitService::LogScope::Head:
                default:
                    showHead->setChecked(true);
                    break;
                }
                return;
            }
            gitService_->setSelectedBranches(picked);
            gitService_->setLogScope(
                services::GitService::LogScope::SelectedBranches);
            if (settingsService_) {
                settingsService_->setValue(
                    "view/logScope", QStringLiteral("filtered"));
                settingsService_->setValue(
                    "view/selectedBranches", picked);
            }
        });
    }
    {
        // Reflog — opens a modeless ReflogDialog showing the
        // reflog for HEAD by default; the combo lets the user
        // pick any local branch's reflog. The action is non-
        // checkable (rather than the original placeholder's
        // checkable toggle) because the dialog itself manages
        // its own visibility — Close hides it. Cmd+Shift+L
        // matches the original placeholder shortcut.
        auto* a = new QAction(menuIcon(QStringLiteral("reflog")),
                              tr("&Reflog..."), this);
        a->setObjectName(QStringLiteral("view.reflog"));
        a->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_L));
        connect(a, &QAction::triggered, this, [this]() {
            if (!gitService_ || !gitService_->isOpen()) return;
            auto* dlg = new dialogs::ReflogDialog(this);
            dlg->setAttribute(Qt::WA_DeleteOnClose);

            // Build the ref list: HEAD first, then every local
            // branch as full ref name (refs/heads/<n>) so the
            // libgit2 reflog read finds the on-disk file.
            QStringList refs;
            refs << QStringLiteral("HEAD");
            if (auto br = gitService_->withRepository(
                    [](git::Repository& r) {
                        return r.branches(git::BranchType::Local);
                    }); br.ok()) {
                for (const auto& b : br.value())
                    refs << QStringLiteral("refs/heads/%1")
                            .arg(QString::fromStdString(b.name));
            }
            // refSelected wiring: every selection re-runs the
            // reflog read and updates the table. Connect first
            // so the initial setRefs() trigger populates the
            // table for HEAD. The dialog is modeless and can
            // outlive a repo switch, so it captures the service —
            // never the Repository*, which dies on the swap — and
            // resolves the live repo per selection under the lock.
            connect(dlg, &dialogs::ReflogDialog::refSelected,
                    dlg, [dlg, svc = gitService_](const QString& ref) {
                auto res = svc->withRepository(
                    [&](git::Repository& r) {
                        return r.reflog(ref.toStdString());
                    });
                if (res.ok()) {
                    dlg->setEntries(res.value());
                } else {
                    // Empty entries on error — typically just
                    // means the ref has no reflog yet (newly
                    // created branch with no operations on it).
                    dlg->setEntries({});
                }
            });

            // Row context-menu recovery actions. Confirmations
            // live here (not in the dialog) so the wording can
            // reference the actual git consequences.
            connect(dlg, &dialogs::ReflogDialog::checkoutRequested,
                    this, [this, dlg](const QString& sha) {
                const auto answer = QMessageBox::question(
                    dlg, tr("Checkout Commit"),
                    tr("Checkout %1?\n\nHEAD will be detached — "
                       "create a branch afterwards if you want to "
                       "keep work based on this state.")
                        .arg(sha.left(8)));
                if (answer != QMessageBox::Yes)
                    return;
                auto out = gitService_->process().run(
                    {"checkout", sha.toStdString()});
                handleProcessResult(dlg, tr("Checkout Failed"), out);
                gitService_->refreshStatus();
                gitService_->refreshLog();
                gitService_->refreshBranches();
                dlg->refreshCurrentRef();
            });
            connect(dlg, &dialogs::ReflogDialog::resetRequested,
                    this, [this, dlg](const QString& sha,
                                      const QString& mode) {
                const QString warning =
                    mode == QLatin1String("hard")
                        ? tr("Reset --hard to %1?\n\nAll uncommitted "
                             "changes in the working tree and index "
                             "will be PERMANENTLY discarded.")
                              .arg(sha.left(8))
                        : tr("Reset --%1 the current branch to %2?")
                              .arg(mode, sha.left(8));
                const auto answer = QMessageBox::question(
                    dlg, tr("Reset Branch"), warning);
                if (answer != QMessageBox::Yes)
                    return;
                auto out = gitService_->process().run(
                    {"reset", "--" + mode.toStdString(),
                     sha.toStdString()});
                handleProcessResult(dlg, tr("Reset Failed"), out);
                gitService_->refreshStatus();
                gitService_->refreshLog();
                gitService_->refreshBranches();
                dlg->refreshCurrentRef();
            });

            dlg->setRefs(refs);
            dlg->show();
        });
        viewMenu->addAction(a);
    }

    viewMenu->addSeparator();
    {
        // Advanced filter -> open AdvancedFilterDialog with the
        // current proxy criteria pre-populated. On Apply, push
        // the new criteria back into the live filter proxy on
        // the revision graph so the table refilters immediately.
        // The toolbar Filter input keeps working in parallel: it
        // drives just messageContains, so users can clear it
        // without losing date/author/SHA filters set here.
        auto* a = new QAction(menuIcon(QStringLiteral("filter")),
                              tr("Ad&vanced filter..."), this);
        a->setObjectName(QStringLiteral("view.advanced-filter"));
        a->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_I));
        connect(a, &QAction::triggered, this, [this]() {
            if (!repoView_ || !repoView_->revisionGraph()) return;
            auto* proxy = repoView_->revisionGraph()->filterProxy();
            if (!proxy) return;
            dialogs::AdvancedFilterDialog dlg(this);
            dlg.setCriteria(proxy->criteria());
            if (dlg.exec() == QDialog::Accepted) {
                proxy->setCriteria(dlg.criteria());
                // Reflect the message-contains criterion in the
                // toolbar input so the user sees what is active.
                if (filterInput_)
                    filterInput_->setText(
                        proxy->criteria().messageContains);
            }
        });
        viewMenu->addAction(a);
    }

    viewMenu->addSeparator();
    //
    // Branch-tree section visibility toggles. Each toggle hides
    // a top-level category (Local/Remote branches, Tags,
    // Submodules, Stashes) in the branch tree on the left.
    // Persisted to QSettings; reapplied to the tree after the
    // repo opens (the model is empty until then).
    //
    // The category index matches BranchModel::RootCategory.
    auto wireBranchTreeToggle = [this, viewMenu](
            const QString& objectName,
            const QString& label,
            const QString& iconKey,
            int categoryIndex,
            const QString& settingsKey,
            const QKeySequence& shortcut = {}) {
        auto* a = new QAction(menuIcon(iconKey), label, this);
        a->setObjectName(objectName);
        a->setCheckable(true);
        if (!shortcut.isEmpty()) a->setShortcut(shortcut);
        const bool saved = settingsService_
            ? settingsService_->value(settingsKey, true).toBool()
            : true;
        a->setChecked(saved);
        connect(a, &QAction::toggled, this,
                [this, categoryIndex, settingsKey](bool on) {
            if (repoView_ && repoView_->branchTree())
                repoView_->branchTree()->setCategoryVisible(
                    categoryIndex, on);
            if (settingsService_)
                settingsService_->setValue(settingsKey, on);
        });
        branchTreeToggles_.push_back({a, categoryIndex});
        viewMenu->addAction(a);
        return a;
    };

    // -- Commits section --
    wireBranchTreeToggle(QStringLiteral("view.show-stashes"),
                         tr("Show s&tashes"),
                         QStringLiteral("stashes"),
                         static_cast<int>(
                             models::BranchModel::RootCategory::Stashes),
                         QStringLiteral("view/showStashes"));
    // Note: a "Show git notes" toggle used to live here as a
    // placeholder. It was removed because the BranchModel doesn't
    // expose a Notes category yet — clicking it just flashed a
    // "not yet implemented" status message, which was misleading.
    // When notes are wired into the model, re-add a real toggle
    // here using wireBranchTreeToggle().

    viewMenu->addSeparator();
    // -- Grid labels section --
    wireBranchTreeToggle(QStringLiteral("view.show-remote-branches"),
                         tr("Show &remote branches"),
                         QStringLiteral("remote"),
                         static_cast<int>(
                             models::BranchModel::RootCategory::RemoteBranches),
                         QStringLiteral("view/showRemoteBranches"),
                         QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_R));
    wireBranchTreeToggle(QStringLiteral("view.show-tags"),
                         tr("Show ta&gs"),
                         QStringLiteral("tag_create"),
                         static_cast<int>(
                             models::BranchModel::RootCategory::Tags),
                         QStringLiteral("view/showTags"),
                         QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_T));

    viewMenu->addSeparator();
    // -- Grid info section --
    {
        // Show commit message body — when on, the Message column
        // renders the full message (summary + body separated by a
        // blank line) instead of the summary-only first line.
        // Useful for users reviewing recent commits without
        // clicking through each one. Persists to settings.
        auto* a = new QAction(menuIcon(QStringLiteral("description")),
                              tr("Show commit &message body"), this);
        a->setObjectName(QStringLiteral("view.show-commit-message-body"));
        a->setCheckable(true);
        const bool saved = settingsService_
            ? settingsService_->value(
                "view/showMessageBody", false).toBool()
            : false;
        a->setChecked(saved);
        if (commitLogModel_)
            commitLogModel_->setShowMessageBody(saved);
        connect(a, &QAction::toggled, this, [this](bool on) {
            if (commitLogModel_) commitLogModel_->setShowMessageBody(on);
            if (settingsService_)
                settingsService_->setValue(
                    "view/showMessageBody", on);
        });
        viewMenu->addAction(a);
    }
    {
        // Show author date vs committer date. The Date column
        // shows the author timestamp by default (matches `git log`
        // behavior). Toggle off to see the committer timestamp,
        // which differs after rebases / amends. Pairs with the
        // relative-date toggle below.
        auto* a = new QAction(menuIcon(QStringLiteral("schedule")),
                              tr("Show a&uthor date"), this);
        a->setObjectName(QStringLiteral("view.show-author-date"));
        a->setCheckable(true);
        const bool saved = settingsService_
            ? settingsService_->value(
                "view/useAuthorDate", true).toBool()
            : true;
        a->setChecked(saved);
        if (commitLogModel_)
            commitLogModel_->setUseAuthorDate(saved);
        connect(a, &QAction::toggled, this, [this](bool on) {
            if (commitLogModel_) commitLogModel_->setUseAuthorDate(on);
            if (settingsService_)
                settingsService_->setValue("view/useAuthorDate", on);
        });
        viewMenu->addAction(a);
    }
    {
        // Show relative date — switches the Date column from
        // absolute timestamps to "X ago" relative formatting.
        // Persists to settings; applies via the model's
        // setRelativeDate which emits dataChanged for repaint.
        auto* a = new QAction(menuIcon(QStringLiteral("schedule")),
                              tr("Show relati&ve date"), this);
        a->setObjectName(QStringLiteral("view.show-relative-date"));
        a->setCheckable(true);
        const bool saved = settingsService_
            ? settingsService_->value(
                "view/relativeDate", false).toBool()
            : false;
        a->setChecked(saved);
        // Apply the initial state to the model now (model exists
        // since constructor created it) so newly-loaded commits
        // render with the right format from the first paint.
        if (commitLogModel_) commitLogModel_->setRelativeDate(saved);
        connect(a, &QAction::toggled, this, [this](bool on) {
            if (commitLogModel_) commitLogModel_->setRelativeDate(on);
            if (settingsService_)
                settingsService_->setValue("view/relativeDate", on);
        });
        viewMenu->addAction(a);
    }

    viewMenu->addSeparator();
    // -- Columns section --
    //
    // Each toggle directly hides/shows a column on the revision
    // graph table. The toggles are checkable QActions that start
    // checked (every column visible by default). We don't persist
    // the visibility to settings yet — that's a small follow-up;
    // for now the toggles are session-local. Avatar isn't a real
    // column in CommitLogModel today (we only show author NAME),
    // so it's marked as a placeholder still.
    auto wireColumnToggle = [this, viewMenu](
            const QString& objectName,
            const QString& label,
            const QString& iconKey,
            int columnIndex,
            const QString& settingsKey) {
        auto* a = new QAction(menuIcon(iconKey), label, this);
        a->setObjectName(objectName);
        a->setCheckable(true);
        // Restore from settings; default visible. The check state
        // is restored here; the graph itself gets the visibility
        // applied later (in onRepositoryOpened) once the table
        // view actually has a model attached.
        const bool saved = settingsService_
            ? settingsService_->value(settingsKey, true).toBool()
            : true;
        a->setChecked(saved);
        connect(a, &QAction::toggled, this,
                [this, columnIndex, settingsKey](bool on) {
            if (repoView_ && repoView_->revisionGraph())
                repoView_->revisionGraph()->setColumnVisible(
                    columnIndex, on);
            if (settingsService_)
                settingsService_->setValue(settingsKey, on);
        });
        // Remember the (action, column) pair so we can re-apply
        // every toggle's current state after the repo opens.
        columnToggles_.push_back({a, columnIndex});
        viewMenu->addAction(a);
        return a;
    };

    wireColumnToggle(QStringLiteral("view.show-revision-graph-column"),
                     tr("Show revision &graph column"),
                     QStringLiteral("submodule"),
                     static_cast<int>(models::CommitLogColumn::Graph),
                     QStringLiteral("view/showGraphColumn"));
    // Note: a "Show author avatar column" toggle used to live here
    // as a placeholder. Removed — the CommitLogModel doesn't have
    // an avatar column and the toggle just flashed "not yet
    // implemented". When/if an avatar column is added, re-add a
    // wireColumnToggle() call here matching the other entries.
    wireColumnToggle(QStringLiteral("view.show-author-name-column"),
                     tr("Show author &name column"),
                     QStringLiteral("badge"),
                     static_cast<int>(models::CommitLogColumn::Author),
                     QStringLiteral("view/showAuthorColumn"));
    wireColumnToggle(QStringLiteral("view.show-date-column"),
                     tr("Show &date column"),
                     QStringLiteral("date"),
                     static_cast<int>(models::CommitLogColumn::Date),
                     QStringLiteral("view/showDateColumn"));
    wireColumnToggle(QStringLiteral("view.show-sha-1-column"),
                     tr("Show SHA-&1 column"),
                     QStringLiteral("hash"),
                     static_cast<int>(models::CommitLogColumn::Hash),
                     QStringLiteral("view/showHashColumn"));

    // ---- Commands ----
    //
    // Each item gets a Google Material Symbols icon tinted with a
    // semantic color (see resources/icons/menu/*.svg). The helper
    // menuIcon() loads the tinted SVG from the Qt resource system.
    cmdMenu_ = menuBar()->addMenu(tr("&Commands"));
    auto* cmdMenu = cmdMenu_;

    commitAction_ = new QAction(tr("Co&mmit..."), this);
    commitAction_->setObjectName(QStringLiteral("act.commit"));
    commitAction_->setIcon(menuIcon(QStringLiteral("commit")));
    commitAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Space));
    commitAction_->setEnabled(false);
    connect(commitAction_, &QAction::triggered,
            this, &MainWindow::showCommitDialog);
    cmdMenu->addAction(commitAction_);

    {
        // Undo last commit → `git reset --soft HEAD~1`. Keeps the
        // changes staged so the user can immediately re-commit with
        // a fixed message. Confirms first because this modifies
        // history on the current branch.
        auto* a = new QAction(menuIcon(QStringLiteral("undo")),
                              tr("&Undo last commit..."), this);
        a->setObjectName(QStringLiteral("commands.undo-last-commit"));
        connect(a, &QAction::triggered, this, [this]() {
            if (!gitService_ || !gitService_->isOpen()) return;
            const auto ret = QMessageBox::question(
                this, tr("Undo Last Commit"),
                tr("This will move HEAD back one commit, leaving its "
                   "changes staged. Continue?"),
                QMessageBox::Yes | QMessageBox::Cancel,
                QMessageBox::Cancel);
            if (ret != QMessageBox::Yes) return;
            auto out = gitService_->process().run(
                {"reset", "--soft", "HEAD~1"});
            handleProcessResult(this, tr("Undo Failed"), out);
            gitService_->refreshStatus();
            gitService_->refreshLog();
        });
        cmdMenu->addAction(a);
    }

    cmdMenu->addSeparator();

    // -----------------------------------------------------------------
    // Fetch / Pull / Push.
    //
    // GitService::{fetch,pull,push} block the UI thread until the
    // subprocess returns (libgit2 transport calls aren't piped through
    // the AsyncRunner yet). That means a click on these buttons used
    // to look like nothing happened: the icon never changed, no status
    // bar text appeared, and "already up to date" succeeded silently.
    //
    // The wrapper below gives the user four independent cues that
    // their click was registered:
    //   1) wait cursor (visible immediately, even before the message
    //      paints — Qt repaints the cursor synchronously)
    //   2) inline toolbar label: "Fetching from origin…" while running,
    //      "✓ Fetch complete" (green) or "✗ Fetch failed" (red) after
    //   3) the action button itself disables for the duration of the
    //      op so the user sees a state change on the thing they clicked
    //   4) status bar message at the bottom (legacy cue)
    //
    // On failure the operationFailed handler beats us to the status
    // bar with "fetch failed: <err>" — `lastRemoteOpFailed_` records
    // that and we suppress the success message so the failure stays
    // visible for its full 5-second timeout.
    auto runRemoteOp = [this](QAction* sourceAction,
                              const QString& startMsg,
                              const QString& successMsg,
                              std::function<void()> op) {
        lastRemoteOpFailed_ = false;

        // Cancel any pending "clear the label" timer from a previous
        // op — otherwise a quick second click could clear our label
        // mid-op when the old timer fires.
        if (remoteOpClearTimer_ && remoteOpClearTimer_->isActive())
            remoteOpClearTimer_->stop();

        if (remoteOpLabel_) {
            remoteOpLabel_->setStyleSheet(QStringLiteral(
                "QLabel { color: palette(window-text); font-style: italic; }"));
            remoteOpLabel_->setText(startMsg);
        }
        statusBar()->showMessage(startMsg);
        if (sourceAction) sourceAction->setEnabled(false);

        QApplication::setOverrideCursor(Qt::WaitCursor);
        QApplication::processEvents();  // paint the disabled state, the
                                        // label, and the cursor before
                                        // the blocking op runs
        op();
        QApplication::restoreOverrideCursor();

        if (sourceAction) sourceAction->setEnabled(true);

        if (remoteOpLabel_) {
            if (lastRemoteOpFailed_) {
                // The operationFailed handler already set the status-bar
                // text to "fetch failed: <err>" and persists it long
                // enough; mirror it inline in red. Collapse any newlines
                // (git stderr is multi-line: "remote: …\nfatal: …\n") to
                // " | " so the toolbar label stays one row tall, and
                // park the full text in a tooltip in case it elides.
                QString status = statusBar()->currentMessage();
                QString oneLine = status;
                oneLine.replace(QChar('\n'), QStringLiteral(" | "));
                oneLine.replace(QChar('\r'), QString());
                remoteOpLabel_->setStyleSheet(QStringLiteral(
                    "QLabel { color: #c43c3c; font-weight: bold; }"));
                remoteOpLabel_->setText(tr("✗ %1").arg(oneLine));
                remoteOpLabel_->setToolTip(status);
            } else {
                remoteOpLabel_->setStyleSheet(QStringLiteral(
                    "QLabel { color: #2e9c36; font-weight: bold; }"));
                remoteOpLabel_->setText(tr("✓ %1").arg(successMsg));
                remoteOpLabel_->setToolTip(QString());
            }
        }

        if (!lastRemoteOpFailed_)
            statusBar()->showMessage(successMsg, 4000);

        // Clear the inline label after a delay. Lazily construct the
        // timer on first use so the constructor doesn't pay for it.
        if (!remoteOpClearTimer_) {
            remoteOpClearTimer_ = new QTimer(this);
            remoteOpClearTimer_->setSingleShot(true);
            connect(remoteOpClearTimer_, &QTimer::timeout, this, [this]() {
                if (remoteOpLabel_) {
                    remoteOpLabel_->clear();
                    remoteOpLabel_->setStyleSheet(QString());
                }
            });
        }
        remoteOpClearTimer_->start(lastRemoteOpFailed_ ? 8000 : 4000);
    };

    fetchAction_ = new QAction(tr("&Fetch"), this);
    fetchAction_->setObjectName(QStringLiteral("act.fetch"));
    fetchAction_->setIcon(menuIcon(QStringLiteral("fetch")));
    fetchAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Down));
    fetchAction_->setToolTip(tr("Fetch from origin (no merge)"));
    fetchAction_->setStatusTip(tr("Download new commits from origin without merging."));
    fetchAction_->setEnabled(false);
    connect(fetchAction_, &QAction::triggered, this,
            [this, runRemoteOp]() {
        runRemoteOp(fetchAction_,
                    tr("Fetching from origin…"),
                    tr("Fetch complete."),
                    [this]() { gitService_->fetch(); });
    });
    cmdMenu->addAction(fetchAction_);

    pullAction_ = new QAction(tr("Pu&ll"), this);
    pullAction_->setObjectName(QStringLiteral("act.pull"));
    pullAction_->setIcon(menuIcon(QStringLiteral("pull")));
    pullAction_->setToolTip(tr("Pull from origin (fetch + merge)"));
    pullAction_->setStatusTip(tr("Fetch and merge from the tracking branch on origin."));
    pullAction_->setEnabled(false);
    connect(pullAction_, &QAction::triggered, this,
            [this, runRemoteOp]() {
        runRemoteOp(pullAction_,
                    tr("Pulling from origin…"),
                    tr("Pull complete."),
                    [this]() { gitService_->pull("origin", ""); });
    });
    cmdMenu->addAction(pullAction_);

    pushAction_ = new QAction(tr("&Push..."), this);
    pushAction_->setObjectName(QStringLiteral("act.push"));
    pushAction_->setIcon(menuIcon(QStringLiteral("push")));
    pushAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Up));
    pushAction_->setToolTip(tr("Push current branch to origin"));
    pushAction_->setStatusTip(tr("Upload local commits on the current branch to origin."));
    pushAction_->setEnabled(false);
    connect(pushAction_, &QAction::triggered, this,
            [this, runRemoteOp]() {
        runRemoteOp(pushAction_,
                    tr("Pushing to origin…"),
                    tr("Push complete."),
                    [this]() { gitService_->push("origin", ""); });
    });
    cmdMenu->addAction(pushAction_);

    cmdMenu->addSeparator();
    {
        // "Manage stashes" is now a full CRUD dialog:
        //   * lists existing stash entries with apply/pop/drop on
        //     the selected entry
        //   * has a "New stash..." button that round-trips to the
        //     existing StashDialog for save
        //
        // The dialog is modeless so the user can interact with the
        // main window between actions, and we re-populate after
        // every action via stashesReady so the displayed list
        // stays in sync with the repo.
        // Promote the manage-stashes action to a MainWindow member so
        // the toolbar Stash button can share the same QAction (and
        // therefore the same enable state, the same click handler,
        // and the same icon). Saves duplicating the dialog-opening
        // lambda below — toolbar and menu both fire it.
        // Title is "&Stash..." rather than "Manage &stashes..." so the
        // toolbar button reads as just "Stash" (matching the
        // GitExtensions toolbar layout) instead of the full menu name.
        stashAction_ = new QAction(menuIcon(QStringLiteral("stashes")),
                                   tr("&Stash..."), this);
        stashAction_->setObjectName(QStringLiteral("act.stash"));
        stashAction_->setToolTip(tr("Manage stashes (apply / pop / drop / new)"));
        connect(stashAction_, &QAction::triggered, this, [this]() {
            if (!gitService_ || !gitService_->isOpen())
                return;

            auto* dlg = new dialogs::StashManageDialog(this);
            dlg->setAttribute(Qt::WA_DeleteOnClose);

            // Initial population from the synchronous Repository
            // call. Subsequent updates arrive via stashesReady.
            auto initial = gitService_->withRepository(
                [](git::Repository& r) { return r.stashes(); });
            if (initial.ok())
                dlg->setStashes(initial.value());

            // Keep the dialog list in sync as stashes mutate. The
            // connection is owned by the dialog so it tears down
            // automatically on close.
            connect(gitService_, &services::GitService::stashesReady,
                    dlg, [dlg](std::vector<git::StashEntry> s) {
                dlg->setStashes(s);
            });

            connect(dlg, &dialogs::StashManageDialog::applyRequested,
                    this, [this](size_t i) {
                gitService_->stashApply(static_cast<int>(i));
            });
            connect(dlg, &dialogs::StashManageDialog::popRequested,
                    this, [this](size_t i) {
                gitService_->stashPop(static_cast<int>(i));
            });
            connect(dlg, &dialogs::StashManageDialog::dropRequested,
                    this, [this, dlg](size_t i) {
                // Look up the stash message so the confirm shows
                // what the user is about to discard, not just an
                // index. "Drop stash@{0}?" is meaningless to a user
                // who has half a dozen stashes; the message line
                // they wrote is what they remember.
                QString message;
                if (auto res = gitService_->withRepository(
                        [](git::Repository& r) { return r.stashes(); });
                    res.ok()) {
                    for (const auto& s : res.value()) {
                        if (s.index == i) {
                            message = QString::fromStdString(s.message);
                            break;
                        }
                    }
                }
                const QString detail = message.isEmpty()
                    ? tr("Discard stash@{%1}? This cannot be undone.")
                          .arg(i)
                    : tr("Discard stash@{%1}:\n\n  %2\n\n"
                         "This cannot be undone.")
                          .arg(i).arg(message);
                const auto confirm = QMessageBox::question(
                    dlg, tr("Drop Stash"), detail,
                    QMessageBox::Yes | QMessageBox::Cancel,
                    QMessageBox::Cancel);
                if (confirm == QMessageBox::Yes)
                    gitService_->stashDrop(static_cast<int>(i));
            });
            connect(dlg, &dialogs::StashManageDialog::newStashRequested,
                    this, [this, dlg]() {
                dialogs::StashDialog save(dlg);
                if (save.exec() == QDialog::Accepted) {
                    gitService_->stashSave(save.message(),
                                           save.includeUntracked());
                }
            });

            dlg->show();
        });
        cmdMenu->addAction(stashAction_);
    }
    {
        // Reset changes — three-mode picker. QInputDialog::getItem
        // with an editable=false list of {soft, mixed, hard}. Hard
        // is destructive so we gate it behind an extra confirm.
        auto* a = new QAction(menuIcon(QStringLiteral("reset")),
                              tr("&Reset changes..."), this);
        a->setObjectName(QStringLiteral("commands.reset-changes"));
        connect(a, &QAction::triggered, this, [this]() {
            if (!gitService_ || !gitService_->isOpen()) return;
            const QStringList modes = {
                tr("soft — keep staged + working tree"),
                tr("mixed — keep working tree (default)"),
                tr("hard — DESTROY working tree changes"),
            };
            bool ok = false;
            const QString picked = QInputDialog::getItem(
                this, tr("Reset Changes"),
                tr("Reset mode (resets to HEAD):"),
                modes, 1, /*editable=*/false, &ok);
            if (!ok) return;
            std::string flag = "--mixed";
            if (picked.startsWith("soft"))  flag = "--soft";
            if (picked.startsWith("hard"))  flag = "--hard";
            if (flag == "--hard") {
                const auto ret = QMessageBox::warning(
                    this, tr("Hard Reset"),
                    tr("This will permanently discard every uncommitted "
                       "change in the working tree. Continue?"),
                    QMessageBox::Yes | QMessageBox::Cancel,
                    QMessageBox::Cancel);
                if (ret != QMessageBox::Yes) return;
            }
            auto out = gitService_->process().run(
                {"reset", flag, "HEAD"});
            handleProcessResult(this, tr("Reset Failed"), out);
            gitService_->refreshStatus();
        });
        cmdMenu->addAction(a);
    }
    {
        // Clean working directory — `git clean -fd` removes every
        // untracked file and empty directory. Strictly destructive,
        // so show a confirm with an explanation first.
        auto* a = new QAction(menuIcon(QStringLiteral("clean")),
                              tr("Clea&n working directory..."), this);
        a->setObjectName(QStringLiteral("commands.clean-working-directory"));
        connect(a, &QAction::triggered, this, [this]() {
            if (!gitService_ || !gitService_->isOpen()) return;
            const auto ret = QMessageBox::warning(
                this, tr("Clean Working Directory"),
                tr("This will permanently remove every untracked file "
                   "and empty directory from the working tree. "
                   "Ignored files are kept.\n\nContinue?"),
                QMessageBox::Yes | QMessageBox::Cancel,
                QMessageBox::Cancel);
            if (ret != QMessageBox::Yes) return;
            auto out = gitService_->process().run(
                {"clean", "-f", "-d"});
            handleProcessResult(this, tr("Clean Failed"), out);
            gitService_->refreshStatus();
        });
        cmdMenu->addAction(a);
    }

    cmdMenu->addSeparator();

    // Helper: gather local branch names as a QStringList for
    // picker dialogs. Empty list if the repo query fails or no
    // repo is open. Captured by reference in the lambdas below.
    auto localBranchNames = [this]() -> QStringList {
        QStringList names;
        if (!gitService_ || !gitService_->isOpen())
            return names;
        auto res = gitService_->withRepository(
            [](git::Repository& r) {
                return r.branches(git::BranchType::Local);
            });
        if (!res.ok()) return names;
        for (const auto& b : res.value())
            names << QString::fromStdString(b.name);
        return names;
    };

    {
        auto* a = new QAction(menuIcon(QStringLiteral("branch_create")),
                              tr("Create &branch..."), this);
        a->setObjectName(QStringLiteral("commands.create-branch"));
        a->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_B));
        connect(a, &QAction::triggered, this, [this]() {
            if (!gitService_ || !gitService_->isOpen()) return;
            bool ok = false;
            const QString name = QInputDialog::getText(
                this, tr("Create Branch"),
                tr("New branch name:"), QLineEdit::Normal,
                QString(), &ok);
            if (ok && !name.trimmed().isEmpty())
                gitService_->createBranch(name.trimmed());
        });
        cmdMenu->addAction(a);
    }
    {
        auto* a = new QAction(menuIcon(QStringLiteral("branch_delete")),
                              tr("&Delete branch..."), this);
        a->setObjectName(QStringLiteral("commands.delete-branch"));
        connect(a, &QAction::triggered, this, [this]() {
            if (!gitService_ || !gitService_->isOpen()) return;
            // Filter out the currently-checked-out branch — git
            // won't let you delete it and offering it to the user
            // is confusing. `branches(Local)` flags HEAD with
            // isHead = true.
            auto branchesRes = gitService_->withRepository(
                [](git::Repository& r) {
                    return r.branches(git::BranchType::Local);
                });
            if (!branchesRes.ok()) return;
            QStringList names;
            for (const auto& b : branchesRes.value()) {
                if (!b.isHead)
                    names << QString::fromStdString(b.name);
            }
            if (names.isEmpty()) {
                QMessageBox::information(this, tr("Delete Branch"),
                    tr("No deletable branches. You can't delete the "
                       "currently-checked-out branch."));
                return;
            }
            bool ok = false;
            const QString picked = QInputDialog::getItem(
                this, tr("Delete Branch"),
                tr("Select a branch to delete:"),
                names, 0, /*editable=*/false, &ok);
            if (!ok || picked.isEmpty()) return;
            const auto confirm = QMessageBox::question(
                this, tr("Delete Branch"),
                tr("Delete branch \"%1\"? This cannot be undone from "
                   "the UI.").arg(picked),
                QMessageBox::Yes | QMessageBox::Cancel,
                QMessageBox::Cancel);
            if (confirm == QMessageBox::Yes)
                gitService_->deleteBranch(picked);
        });
        cmdMenu->addAction(a);
    }
    {
        auto* a = new QAction(menuIcon(QStringLiteral("branch_checkout")),
                              tr("Check&out branch..."), this);
        a->setObjectName(QStringLiteral("commands.checkout-branch"));
        connect(a, &QAction::triggered, this, [this, localBranchNames]() {
            if (!gitService_ || !gitService_->isOpen()) return;
            const QStringList names = localBranchNames();
            if (names.isEmpty()) return;
            bool ok = false;
            const QString picked = QInputDialog::getItem(
                this, tr("Checkout Branch"),
                tr("Select a branch to check out:"),
                names, 0, /*editable=*/false, &ok);
            if (ok && !picked.isEmpty())
                gitService_->checkoutBranch(picked);
        });
        cmdMenu->addAction(a);
    }
    {
        // Merge: run `git merge <branch>` via GitProcess so we
        // don't have to resolve the branch name to an ObjectId
        // first (Repository::merge takes a raw ObjectId). The CLI
        // path also gives us git's standard conflict semantics
        // without needing to teach the UI about MergeResult cases.
        // The current HEAD branch is filtered out of the picker —
        // merging a branch into itself is a no-op and surprises
        // the user.
        auto* a = new QAction(menuIcon(QStringLiteral("merge")),
                              tr("Mer&ge branches..."), this);
        a->setObjectName(QStringLiteral("commands.merge-branches"));
        a->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_M));
        connect(a, &QAction::triggered, this, [this]() {
            if (!gitService_ || !gitService_->isOpen()) return;
            auto branchesRes = gitService_->withRepository(
                [](git::Repository& r) {
                    return r.branches(git::BranchType::Local);
                });
            if (!branchesRes.ok()) return;
            QStringList names;
            for (const auto& b : branchesRes.value()) {
                if (!b.isHead)
                    names << QString::fromStdString(b.name);
            }
            if (names.isEmpty()) return;
            bool ok = false;
            const QString picked = QInputDialog::getItem(
                this, tr("Merge Branch"),
                tr("Merge which branch into the current one?"),
                names, 0, /*editable=*/false, &ok);
            if (!ok || picked.isEmpty()) return;
            auto out = gitService_->process().run(
                {"merge", picked.toStdString()});
            const bool merged =
                handleProcessResult(this, tr("Merge Failed"), out);
            gitService_->refreshStatus();
            gitService_->refreshLog();
            gitService_->refreshBranches();
            if (!merged)
                offerConflictResolution(tr("merge"));
        });
        cmdMenu->addAction(a);
    }
    {
        // Resolve conflicts — opens the three-way resolver for
        // whatever conflicted operation is in progress (merge,
        // cherry-pick, rebase, revert). Also offered automatically
        // when a merge / cherry-pick stops on conflicts.
        auto* a = new QAction(menuIcon(QStringLiteral("merge")),
                              tr("Resolve co&nflicts..."), this);
        a->setObjectName(QStringLiteral("commands.resolve-conflicts"));
        connect(a, &QAction::triggered,
                this, &MainWindow::showConflictResolver);
        cmdMenu->addAction(a);
    }
    {
        // Rebase — two-stage flow with RebaseDialog.
        //   Stage 1: open the dialog with the local branch list
        //            populated. The dialog emits targetRefChanged
        //            as the user picks (or types) a target.
        //   Stage 2: resolve the target ref to an ObjectId, walk
        //            the commits reachable from HEAD but not from
        //            the target (this is the set that would be
        //            replayed onto the target), and feed that list
        //            back via setCommitsToRebase. The dialog
        //            renders the preview and lets the user assign
        //            per-commit operations.
        //   Stage 3: on rebaseRequested(plan) we hand off to
        //            GitService::interactiveRebase. Outcome
        //            arrives async on rebaseComplete; the status
        //            bar surfaces the result.
        auto* a = new QAction(menuIcon(QStringLiteral("rebase")),
                              tr("R&ebase..."), this);
        a->setObjectName(QStringLiteral("commands.rebase"));
        a->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E));
        connect(a, &QAction::triggered, this, [this]() {
            if (!gitService_ || !gitService_->isOpen()) return;

            auto* dlg = new dialogs::RebaseDialog(this);
            dlg->setAttribute(Qt::WA_DeleteOnClose);

            // Branch picker: every local branch except HEAD —
            // rebasing onto your own branch is a no-op and
            // confuses the preview.
            std::vector<git::BranchInfo> branches;
            if (auto res = gitService_->withRepository(
                    [](git::Repository& r) {
                        return r.branches(git::BranchType::Local);
                    }); res.ok()) {
                for (const auto& b : res.value()) {
                    if (!b.isHead) branches.push_back(b);
                }
            }
            dlg->setBranches(branches);

            // The dialog is modeless and survives repo switches, so
            // the preview lambda captures the service — never a raw
            // Repository*, which the swap destroys — and resolves
            // the live repo per invocation, holding the repo lock
            // for the whole resolve-and-walk so refresh workers
            // can't interleave.
            connect(dlg, &dialogs::RebaseDialog::targetRefChanged,
                    dlg, [dlg, svc = gitService_](const QString& ref) {
                if (ref.trimmed().isEmpty()) return;
                struct RebasePreview {
                    std::vector<git::CommitData> commits;
                    git::ObjectId onto;
                    bool valid = false;
                };
                const auto preview = svc->withRepository(
                    [&](git::Repository& repo) {
                        RebasePreview p;
                        // Resolve the user-entered target → ObjectId.
                        auto onto = repo.resolveRef(ref.toStdString());
                        if (!onto.ok()) return p;

                        // Walk commits reachable from HEAD but not
                        // from the target — the canonical "what
                        // would be replayed" set. RevWalk's hide()
                        // excludes a commit and its ancestors from
                        // the walk; we also push HEAD as the
                        // starting point.
                        auto walkRes = repo.createRevWalk();
                        if (!walkRes.ok()) return p;
                        auto& walk = walkRes.value();
                        walk.setSorting(git::SortOrder::TopologicalTime);
                        if (!walk.pushHead().ok()) return p;
                        if (!walk.hide(onto.value()).ok()) return p;
                        auto commitsRes = walk.all();
                        if (!commitsRes.ok()) return p;
                        p.commits = std::move(commitsRes).value();
                        p.onto = onto.value();
                        p.valid = true;
                        return p;
                    });
                if (preview.valid)
                    dlg->setCommitsToRebase(preview.commits,
                                            preview.onto);
            });

            connect(dlg, &dialogs::RebaseDialog::rebaseRequested,
                    this, [this](const git::RebasePlan& plan) {
                gitService_->interactiveRebase(plan);
            });

            // Conflict-resolution controls: the embedded widget
            // enables Continue / Skip / Abort once a rebase has
            // started, and the dialog is shown non-modally so it
            // stays available while the rebase is paused.
            connect(dlg, &dialogs::RebaseDialog::rebaseContinueRequested,
                    gitService_, &services::GitService::rebaseContinue);
            connect(dlg, &dialogs::RebaseDialog::rebaseSkipRequested,
                    gitService_, &services::GitService::rebaseSkip);
            connect(dlg, &dialogs::RebaseDialog::rebaseAbortRequested,
                    gitService_, &services::GitService::rebaseAbort);

            // rebaseComplete is async — surface to the status bar
            // so the user knows whether it ran cleanly. Connection
            // is owned by `this` so it persists across rebases;
            // we use a unique signal so reconnecting is harmless.
            // (StatusBar message lives 4s, then the standard
            // "Ready" reasserts.)
            connect(gitService_, &services::GitService::rebaseComplete,
                    this, [this](bool success) {
                statusBar()->showMessage(
                    success ? tr("Rebase complete.")
                            : tr("Rebase paused or failed — "
                                 "use Continue/Abort to resolve."),
                    4000);
                gitService_->refreshLog();
                gitService_->refreshStatus();
                gitService_->refreshBranches();
            }, Qt::UniqueConnection);

            dlg->show();
        });
        cmdMenu->addAction(a);
    }

    cmdMenu->addSeparator();
    {
        auto* a = new QAction(menuIcon(QStringLiteral("tag_create")),
                              tr("Create &tag..."), this);
        a->setObjectName(QStringLiteral("commands.create-tag"));
        a->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_T));
        connect(a, &QAction::triggered, this, [this]() {
            if (!gitService_ || !gitService_->isOpen())
                return;
            dialogs::TagDialog dlg(this);
            QStringList branchNames;
            if (auto res = gitService_->withRepository(
                    [](git::Repository& r) {
                        return r.branches(git::BranchType::Local);
                    }); res.ok()) {
                for (const auto& b : res.value())
                    branchNames << QString::fromStdString(b.name);
            }
            dlg.setBranches(branchNames);
            if (dlg.exec() == QDialog::Accepted) {
                gitService_->createTag(dlg.tagName(),
                                       dlg.targetRef(),
                                       dlg.message(),
                                       dlg.isAnnotated());
            }
        });
        cmdMenu->addAction(a);
    }
    {
        // Simple list-picker dialog rather than a full TagDialog —
        // delete-tag only needs a tag name. Uses QInputDialog::getItem
        // so the user sees existing tag names (from the repo) rather
        // than having to type one from memory. The tag list is
        // resolved synchronously via Repository::tags(); if that call
        // fails we fall back to a free-form text prompt so deletion
        // still works against the off-chance of a libgit2 enumeration
        // hiccup.
        auto* a = new QAction(menuIcon(QStringLiteral("tag_delete")),
                              tr("De&lete tag..."), this);
        a->setObjectName(QStringLiteral("commands.delete-tag"));
        connect(a, &QAction::triggered, this, [this]() {
            if (!gitService_ || !gitService_->isOpen())
                return;
            QStringList tagNames;
            if (auto res = gitService_->withRepository(
                    [](git::Repository& r) { return r.tags(); });
                res.ok()) {
                for (const auto& t : res.value())
                    tagNames << QString::fromStdString(t.name);
            }
            bool ok = false;
            QString picked;
            if (!tagNames.isEmpty()) {
                picked = QInputDialog::getItem(
                    this, tr("Delete Tag"),
                    tr("Select a tag to delete:"),
                    tagNames, 0, /*editable=*/false, &ok);
            } else {
                picked = QInputDialog::getText(
                    this, tr("Delete Tag"),
                    tr("Tag name to delete:"),
                    QLineEdit::Normal, QString(), &ok);
            }
            if (!ok || picked.isEmpty()) return;
            const auto confirm = QMessageBox::question(
                this, tr("Delete Tag"),
                tr("Delete tag \"%1\"?").arg(picked),
                QMessageBox::Yes | QMessageBox::Cancel,
                QMessageBox::Cancel);
            if (confirm == QMessageBox::Yes)
                gitService_->deleteTag(picked);
        });
        cmdMenu->addAction(a);
    }

    cmdMenu->addSeparator();
    {
        // CherryPickDialog emits commitHashChanged as the user types,
        // and expects the host to resolve the entered hash to a commit
        // preview via setCommitDetails(). We parse the text as a full
        // 40-char hex sha via ObjectId::fromHex and look it up; short
        // hashes and ref names aren't supported yet (no ref-resolver
        // on Repository — Phase 2 polish). If the hash is malformed
        // or doesn't resolve we just clear the preview; OK stays
        // enabled and GitService will surface the failure.
        auto* a = new QAction(menuIcon(QStringLiteral("cherry_pick")),
                              tr("C&herry pick..."), this);
        a->setObjectName(QStringLiteral("commands.cherry-pick"));
        connect(a, &QAction::triggered, this, [this]() {
            if (!gitService_ || !gitService_->isOpen())
                return;
            auto* dlg = new dialogs::CherryPickDialog(this);
            dlg->setAttribute(Qt::WA_DeleteOnClose);

            // Use Repository::resolveRef so the user can paste full
            // SHAs, short SHAs (>=4 hex), branch names, tags,
            // HEAD~3, etc. — anything libgit2's revparse accepts.
            // The dialog is modeless, so these lambdas capture the
            // service — never a raw Repository*, which a repo switch
            // destroys — and resolve the live repo under its lock.
            auto resolve = [svc = gitService_](const QString& spec)
                    -> std::optional<git::ObjectId> {
                const QString trimmed = spec.trimmed();
                if (trimmed.isEmpty()) return std::nullopt;
                auto res = svc->withRepository(
                    [&](git::Repository& r) {
                        return r.resolveRef(trimmed.toStdString());
                    });
                if (!res.ok()) return std::nullopt;
                return res.value();
            };

            connect(dlg, &dialogs::CherryPickDialog::commitHashChanged,
                    dlg, [dlg, svc = gitService_](const QString& hash) {
                const QString trimmed = hash.trimmed();
                if (trimmed.isEmpty()) {
                    dlg->clearCommitDetails();
                    return;
                }
                // Resolve + commit lookup under one lock.
                auto cmtRes = svc->withRepository(
                    [&](git::Repository& r)
                        -> git::Result<git::CommitData> {
                        auto oid = r.resolveRef(trimmed.toStdString());
                        if (!oid.ok()) return oid.error();
                        return r.lookupCommit(oid.value());
                    });
                if (!cmtRes.ok()) {
                    dlg->clearCommitDetails();
                    return;
                }
                dlg->setCommitDetails(cmtRes.value());
            });

            connect(dlg, &QDialog::accepted, this,
                    [this, dlg, resolve]() {
                auto oid = resolve(dlg->commitHash());
                if (!oid) {
                    QMessageBox::information(this, tr("Cherry Pick"),
                        tr("Couldn't resolve that revision. Try a "
                           "full or short SHA, branch name, or tag."));
                    return;
                }
                gitService_->cherryPick({*oid});
            });

            dlg->open();
        });
        cmdMenu->addAction(a);
    }
    {
        // Archive revision → `git archive --format=zip <ref> -o <file>`.
        // Prompt for the revision (default HEAD) and the output file;
        // format is picked from the output extension (.zip or .tar).
        auto* a = new QAction(menuIcon(QStringLiteral("archive")),
                              tr("&Archive revision..."), this);
        a->setObjectName(QStringLiteral("commands.archive-revision"));
        connect(a, &QAction::triggered, this, [this]() {
            if (!gitService_ || !gitService_->isOpen()) return;
            bool ok = false;
            const QString revision = QInputDialog::getText(
                this, tr("Archive Revision"),
                tr("Revision (ref, branch, or sha — default HEAD):"),
                QLineEdit::Normal, QStringLiteral("HEAD"), &ok);
            if (!ok) return;
            const QString ref = revision.trimmed().isEmpty()
                ? QStringLiteral("HEAD") : revision.trimmed();
            const QString outFile = QFileDialog::getSaveFileName(
                this, tr("Save Archive As"), QDir::homePath(),
                tr("Zip archive (*.zip);;Tar archive (*.tar)"));
            if (outFile.isEmpty()) return;
            const std::string format = outFile.endsWith(".tar")
                ? "tar" : "zip";
            auto out = gitService_->process().run(
                {"archive", "--format=" + format,
                 "-o", outFile.toStdString(),
                 ref.toStdString()});
            handleProcessResult(this, tr("Archive Failed"), out);
        });
        cmdMenu->addAction(a);
    }
    {
        // Checkout revision → Repository::checkout takes any
        // refspec (branch, tag, sha). We warn about detached HEAD
        // state first so the user isn't surprised if the ref isn't
        // a local branch.
        auto* a = new QAction(menuIcon(QStringLiteral("checkout")),
                              tr("Checko&ut revision..."), this);
        a->setObjectName(QStringLiteral("commands.checkout-revision"));
        connect(a, &QAction::triggered, this, [this]() {
            if (!gitService_ || !gitService_->isOpen()) return;
            bool ok = false;
            const QString ref = QInputDialog::getText(
                this, tr("Checkout Revision"),
                tr("Revision to check out (ref, tag, or sha):"),
                QLineEdit::Normal, QString(), &ok);
            if (!ok || ref.trimmed().isEmpty()) return;
            const auto confirm = QMessageBox::question(
                this, tr("Checkout Revision"),
                tr("Check out %1?\n\nIf this isn't a branch name "
                   "you'll be in detached-HEAD state.")
                    .arg(ref.trimmed()),
                QMessageBox::Yes | QMessageBox::Cancel,
                QMessageBox::Cancel);
            if (confirm != QMessageBox::Yes) return;
            auto res = gitService_->withRepository(
                [&](git::Repository& r) {
                    return r.checkout(ref.trimmed().toStdString());
                });
            if (!res.ok()) {
                QMessageBox::warning(this, tr("Checkout Failed"),
                    QString::fromStdString(res.error().message()));
            }
            gitService_->refreshStatus();
            gitService_->refreshLog();
            gitService_->refreshBranches();
        });
        cmdMenu->addAction(a);
    }

    cmdMenu->addSeparator();
    {
        // Bisect — submenu with five entries (start, good, bad,
        // skip, reset). The user runs `git bisect start <bad>
        // <good>` to begin, then iteratively marks the current
        // checkout as good or bad; git checks out the midpoint
        // each time and converges in log2(N) steps. Reset
        // unwinds the bisect state and returns to the original
        // branch.
        //
        // We don't try to model bisect state in the UI today —
        // the user just runs the actions and watches the status
        // bar / refreshed log. A future improvement would be a
        // dedicated panel showing "%d revs left" and the current
        // suspect commit.
        auto* sub = cmdMenu->addMenu(menuIcon(QStringLiteral("bisect")),
                                     tr("&Bisect"));
        auto runBisect = [this](const QStringList& args,
                                const QString& failTitle) {
            if (!gitService_ || !gitService_->isOpen()) return;
            std::vector<std::string> stdArgs = {"bisect"};
            for (const auto& a : args) stdArgs.push_back(a.toStdString());
            auto out = gitService_->process().run(stdArgs);
            handleProcessResult(this, failTitle, out);
            gitService_->refreshStatus();
            gitService_->refreshLog();
        };

        auto* startA = new QAction(tr("&Start..."), this);
        startA->setObjectName(QStringLiteral("commands.bisect.start"));
        connect(startA, &QAction::triggered, this, [this, runBisect]() {
            if (!gitService_ || !gitService_->isOpen()) return;
            bool ok = false;
            const QString badRev = QInputDialog::getText(
                this, tr("Bisect Start"),
                tr("Bad commit (where the bug exists, default HEAD):"),
                QLineEdit::Normal, QStringLiteral("HEAD"), &ok);
            if (!ok) return;
            const QString goodRev = QInputDialog::getText(
                this, tr("Bisect Start"),
                tr("Good commit (where the bug doesn't exist):"),
                QLineEdit::Normal, QString(), &ok);
            if (!ok || goodRev.trimmed().isEmpty()) return;
            runBisect({"start",
                       badRev.trimmed().isEmpty() ? "HEAD"
                                                  : badRev.trimmed(),
                       goodRev.trimmed()},
                      tr("Bisect Start Failed"));
        });
        sub->addAction(startA);

        auto* goodA = new QAction(tr("Mark current as &good"), this);
        goodA->setObjectName(QStringLiteral("commands.bisect.mark-current-as-good"));
        connect(goodA, &QAction::triggered, this,
                [runBisect]() { runBisect({"good"},
                                          QObject::tr("Bisect Good Failed")); });
        sub->addAction(goodA);

        auto* badA = new QAction(tr("Mark current as &bad"), this);
        badA->setObjectName(QStringLiteral("commands.bisect.mark-current-as-bad"));
        connect(badA, &QAction::triggered, this,
                [runBisect]() { runBisect({"bad"},
                                          QObject::tr("Bisect Bad Failed")); });
        sub->addAction(badA);

        auto* skipA = new QAction(tr("S&kip current"), this);
        skipA->setObjectName(QStringLiteral("commands.bisect.skip-current"));
        connect(skipA, &QAction::triggered, this,
                [runBisect]() { runBisect({"skip"},
                                          QObject::tr("Bisect Skip Failed")); });
        sub->addAction(skipA);

        sub->addSeparator();
        auto* resetA = new QAction(tr("&Reset"), this);
        resetA->setObjectName(QStringLiteral("commands.bisect.reset"));
        connect(resetA, &QAction::triggered, this,
                [runBisect]() { runBisect({"reset"},
                                          QObject::tr("Bisect Reset Failed")); });
        sub->addAction(resetA);
    }

    cmdMenu->addSeparator();
    {
        // Format patch → `git format-patch <range> -o <dir>`. We
        // prompt for the revision range (default `origin/main..HEAD`
        // — what most users want when they're prepping a series for
        // mailing-list review) and the output directory. Files end
        // up named 0001-foo.patch, 0002-bar.patch, etc.
        auto* a = new QAction(menuIcon(QStringLiteral("format_patch")),
                              tr("&Format patch..."), this);
        a->setObjectName(QStringLiteral("commands.format-patch"));
        connect(a, &QAction::triggered, this, [this]() {
            if (!gitService_ || !gitService_->isOpen()) return;
            bool ok = false;
            const QString range = QInputDialog::getText(
                this, tr("Format Patch"),
                tr("Revision range (e.g. origin/main..HEAD, "
                   "HEAD~5..HEAD):"),
                QLineEdit::Normal,
                QStringLiteral("origin/main..HEAD"), &ok);
            if (!ok || range.trimmed().isEmpty()) return;
            const QString outDir = QFileDialog::getExistingDirectory(
                this, tr("Choose output directory for patches"),
                QDir::homePath());
            if (outDir.isEmpty()) return;
            auto out = gitService_->process().run(
                {"format-patch", range.trimmed().toStdString(),
                 "-o", outDir.toStdString()});
            handleProcessResult(this, tr("Format Patch Failed"), out);
            if (out.ok() && out.value().success()) {
                statusBar()->showMessage(
                    tr("Patches written to %1").arg(outDir), 5000);
            }
        });
        cmdMenu->addAction(a);
    }
    {
        // Apply patch → `git am <files>` or `git apply <files>`.
        // We use `git am` so the commit metadata in the patch is
        // preserved (author / date / subject / message). For raw
        // diffs without commit metadata the user wants `git apply`,
        // which we expose as a checkbox. Multiple files are
        // accepted in one go and applied in order.
        auto* a = new QAction(menuIcon(QStringLiteral("apply_patch")),
                              tr("A&pply patch..."), this);
        a->setObjectName(QStringLiteral("commands.apply-patch"));
        connect(a, &QAction::triggered, this, [this]() {
            if (!gitService_ || !gitService_->isOpen()) return;
            const QStringList files = QFileDialog::getOpenFileNames(
                this, tr("Choose patch files"), QDir::homePath(),
                tr("Patch files (*.patch *.diff *.mbox);;All files (*)"));
            if (files.isEmpty()) return;

            // Choose between `am` (mailbox-style with metadata) and
            // `apply` (raw diff). Default to `am` for .patch and
            // .mbox; default to `apply` for .diff. The user can
            // override via a question dialog.
            bool useAm = true;
            for (const auto& f : files) {
                if (f.endsWith(".diff", Qt::CaseInsensitive)) {
                    useAm = false;
                    break;
                }
            }
            const auto picked = QMessageBox::question(this,
                tr("Apply Patch"),
                tr("Apply with `git am` (preserves commit metadata) "
                   "or `git apply` (raw diff, no commit)?\n\n"
                   "Default suggestion based on file extensions: %1")
                    .arg(useAm ? "git am" : "git apply"),
                QMessageBox::Yes | QMessageBox::No |
                    QMessageBox::Cancel,
                useAm ? QMessageBox::Yes : QMessageBox::No);
            if (picked == QMessageBox::Cancel) return;
            useAm = (picked == QMessageBox::Yes);

            std::vector<std::string> args;
            args.push_back(useAm ? "am" : "apply");
            for (const auto& f : files) args.push_back(f.toStdString());
            auto out = gitService_->process().run(
                args, /*timeout=*/120000);
            handleProcessResult(this, tr("Apply Patch Failed"), out);
            gitService_->refreshStatus();
            gitService_->refreshLog();
        });
        cmdMenu->addAction(a);
    }

    // ---- Plugins ----
    auto* pluginsMenu = menuBar()->addMenu(tr("&Plugins"));
    {
        // "Delete obsolete branches" — show every local branch
        // that has been merged into the current HEAD (so deleting
        // it loses no commits). Powered by `git branch --merged`,
        // which prints every branch reachable from HEAD; we trim
        // the leading marker chars and drop the current HEAD line.
        // The user picks branches to delete via a multi-select
        // QListWidget dialog.
        auto* a = new QAction(menuIcon(QStringLiteral("auto_delete")),
                              tr("&Delete obsolete branches..."), this);
        a->setObjectName(QStringLiteral("plugins.delete-obsolete-branches"));
        connect(a, &QAction::triggered, this, [this]() {
            if (!gitService_ || !gitService_->isOpen()) return;
            auto out = gitService_->process().run(
                {"branch", "--merged"});
            if (!out.ok() || !out.value().success()) {
                handleProcessResult(this, tr("Delete Obsolete Failed"),
                                    out);
                return;
            }
            QStringList merged;
            for (const auto& raw : QString::fromStdString(
                    out.value().stdoutData).split('\n',
                        Qt::SkipEmptyParts)) {
                QString line = raw.trimmed();
                if (line.startsWith('*')) continue;  // current HEAD
                if (line.startsWith('+')) line = line.mid(1).trimmed();
                line = line.trimmed();
                // Skip detached-HEAD pseudo entries like "(HEAD ...)"
                if (line.startsWith('(')) continue;
                if (!line.isEmpty()) merged << line;
            }
            if (merged.isEmpty()) {
                QMessageBox::information(this,
                    tr("Delete Obsolete Branches"),
                    tr("No merged branches to clean up."));
                return;
            }

            // Multi-select picker. QInputDialog::getItem only
            // does single-select, so we build a small QDialog
            // hosting a QListWidget with multi-selection enabled.
            QDialog dlg(this);
            dlg.setWindowTitle(tr("Delete Obsolete Branches"));
            dlg.resize(420, 360);
            auto* layout = new QVBoxLayout(&dlg);
            layout->addWidget(new QLabel(
                tr("Select branches that have been merged into HEAD "
                   "and should be deleted:"), &dlg));
            auto* list = new QListWidget(&dlg);
            list->setSelectionMode(QAbstractItemView::ExtendedSelection);
            for (const auto& n : merged) list->addItem(n);
            layout->addWidget(list);
            auto* buttons = new QDialogButtonBox(
                QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                &dlg);
            connect(buttons, &QDialogButtonBox::accepted,
                    &dlg, &QDialog::accept);
            connect(buttons, &QDialogButtonBox::rejected,
                    &dlg, &QDialog::reject);
            layout->addWidget(buttons);
            if (dlg.exec() != QDialog::Accepted) return;

            QStringList picked;
            for (auto* it : list->selectedItems())
                picked << it->text();
            if (picked.isEmpty()) return;

            const auto confirm = QMessageBox::question(this,
                tr("Delete Obsolete Branches"),
                tr("Delete %1 merged branch(es)? This cannot be "
                   "undone from the UI.").arg(picked.size()),
                QMessageBox::Yes | QMessageBox::Cancel,
                QMessageBox::Cancel);
            if (confirm != QMessageBox::Yes) return;
            for (const auto& n : picked)
                gitService_->deleteBranch(n);
        });
        pluginsMenu->addAction(a);
    }
    {
        // "Find large files" — walk every blob in the repo via
        // `git rev-list --objects --all` plumbed through
        // `git cat-file --batch-check`, sort by size, show the
        // top 50 in a read-only table. Surfaces files that bloat
        // the pack (CI artifacts checked in by mistake, big PSDs,
        // accidentally-committed videos). Filter by min size to
        // skip noise.
        auto* a = new QAction(menuIcon(QStringLiteral("find_files")),
                              tr("&Find large files..."), this);
        a->setObjectName(QStringLiteral("plugins.find-large-files"));
        connect(a, &QAction::triggered, this, [this]() {
            if (!gitService_ || !gitService_->isOpen()) return;

            // The pipeline runs git twice: first emit object IDs,
            // then resolve each to (size, type) via cat-file.
            // We pipe with QProcess channels rather than a shell
            // because users may not have a POSIX shell on PATH
            // (Windows CI). Output is small enough to buffer in
            // memory — a million blobs is ~50 MB of text.
            //
            // The work runs on the main thread today and can take
            // a few seconds on big repos. Show a wait cursor +
            // status bar message so the user knows the app hasn't
            // hung. Future improvement: run on QtConcurrent.
            QApplication::setOverrideCursor(Qt::WaitCursor);
            statusBar()->showMessage(
                tr("Walking repository for large blobs..."));
            // RAII-style guard so the cursor and status bar reset
            // even if we early-return on error.
            struct ScopeGuard {
                QStatusBar* sb;
                ~ScopeGuard() {
                    QApplication::restoreOverrideCursor();
                    if (sb) sb->clearMessage();
                }
            } guard{statusBar()};

            // GitService::process() locks internally and returns
            // the GitProcess by value; shell calls then run without
            // touching libgit2 state.
            auto proc = gitService_->process();
            auto revRes = proc.run(
                {"rev-list", "--objects", "--all"}, /*timeout=*/120000);
            if (!revRes.ok() || !revRes.value().success()) {
                handleProcessResult(this, tr("Find Large Files Failed"),
                                    revRes);
                return;
            }

            // Build the cat-file input: just the SHAs (one per
            // line). Each rev-list line is "<sha> <path>" with
            // path optional for non-blob types.
            QStringList shaLines;
            QHash<QString, QString> shaToPath;
            for (const auto& raw : QString::fromStdString(
                    revRes.value().stdoutData).split('\n',
                        Qt::SkipEmptyParts)) {
                const qsizetype sp = raw.indexOf(' ');
                if (sp <= 0) continue;
                const QString sha = raw.left(sp);
                const QString path = raw.mid(sp + 1);
                shaLines << sha;
                if (!path.isEmpty()) shaToPath[sha] = path;
            }

            QProcess cat;
            cat.setWorkingDirectory(QString::fromStdString(
                gitService_->withRepository([](git::Repository& r) {
                    return r.workdir();
                })));
            cat.start("git", QStringList{
                "cat-file", "--batch-check=%(objectname) "
                            "%(objecttype) %(objectsize)"});
            if (!cat.waitForStarted(5000)) {
                QMessageBox::warning(this,
                    tr("Find Large Files Failed"),
                    tr("Could not start `git cat-file`."));
                return;
            }
            cat.write(shaLines.join('\n').toUtf8());
            cat.write("\n");
            cat.closeWriteChannel();
            if (!cat.waitForFinished(120000)) {
                cat.kill();
                QMessageBox::warning(this,
                    tr("Find Large Files Failed"),
                    tr("`git cat-file` timed out."));
                return;
            }

            struct Entry { QString path; QString sha; qint64 size; };
            std::vector<Entry> entries;
            entries.reserve(static_cast<size_t>(shaLines.size()));
            for (const auto& raw : QString::fromUtf8(
                    cat.readAllStandardOutput()).split('\n',
                        Qt::SkipEmptyParts)) {
                const QStringList p = raw.split(' ');
                if (p.size() < 3) continue;
                if (p[1] != QLatin1String("blob")) continue;
                bool ok = false;
                const qint64 sz = p[2].toLongLong(&ok);
                if (!ok) continue;
                entries.push_back({shaToPath.value(p[0]), p[0], sz});
            }
            std::sort(entries.begin(), entries.end(),
                [](const Entry& a, const Entry& b) {
                    return a.size > b.size;
                });
            if (entries.size() > 50) entries.resize(50);

            QDialog dlg(this);
            dlg.setWindowTitle(tr("Largest Blobs in Repository"));
            dlg.resize(720, 460);
            auto* layout = new QVBoxLayout(&dlg);
            layout->addWidget(new QLabel(tr(
                "Top %1 largest blobs across all branches and "
                "history. Sizes are uncompressed object sizes; "
                "the on-disk pack will be smaller.")
                    .arg(entries.size()), &dlg));
            auto* table = new QTableWidget(
                static_cast<int>(entries.size()), 3, &dlg);
            table->setHorizontalHeaderLabels(
                {tr("Size"), tr("Path"), tr("Blob SHA")});
            table->horizontalHeader()->setStretchLastSection(false);
            table->horizontalHeader()->setSectionResizeMode(
                1, QHeaderView::Stretch);
            table->setEditTriggers(QAbstractItemView::NoEditTriggers);
            table->setAlternatingRowColors(true);
            table->verticalHeader()->setVisible(false);
            for (size_t i = 0; i < entries.size(); ++i) {
                const auto& e = entries[i];
                // Human-readable size: KB / MB / GB. Locale-aware
                // formatting for the number part.
                QString humanSize;
                if (e.size >= 1024LL * 1024 * 1024)
                    humanSize = QStringLiteral("%1 GB").arg(
                        e.size / (1024.0 * 1024 * 1024), 0, 'f', 2);
                else if (e.size >= 1024 * 1024)
                    humanSize = QStringLiteral("%1 MB").arg(
                        e.size / (1024.0 * 1024), 0, 'f', 2);
                else if (e.size >= 1024)
                    humanSize = QStringLiteral("%1 KB").arg(
                        e.size / 1024.0, 0, 'f', 1);
                else
                    humanSize = QStringLiteral("%1 B").arg(e.size);
                auto* sizeItem = new QTableWidgetItem(humanSize);
                sizeItem->setData(Qt::UserRole,
                                  static_cast<qulonglong>(e.size));
                table->setItem(static_cast<int>(i), 0, sizeItem);
                table->setItem(static_cast<int>(i), 1,
                    new QTableWidgetItem(e.path));
                table->setItem(static_cast<int>(i), 2,
                    new QTableWidgetItem(e.sha.left(12)));
            }
            layout->addWidget(table);
            auto* close = new QDialogButtonBox(
                QDialogButtonBox::Close, &dlg);
            connect(close, &QDialogButtonBox::rejected,
                    &dlg, &QDialog::accept);
            layout->addWidget(close);
            dlg.exec();
        });
        pluginsMenu->addAction(a);
    }
    {
        auto* a = new QAction(menuIcon(QStringLiteral("submodule")),
                              tr("&GitFlow"), this);
        a->setObjectName(QStringLiteral("plugins.gitflow"));
        connect(a, &QAction::triggered, this, [this]() {
            if (!gitService_ || !gitService_->isOpen())
                return;
            dialogs::GitFlowDialog dlg(gitService_, this);
            dlg.showSetupWizardIfNeeded();
            dlg.exec();
        });
        pluginsMenu->addAction(a);
    }
    // Note: an "Impact Graph" entry used to live here as a
    // placeholder. Removed — clicking it only flashed
    // "not yet implemented", which was misleading. Re-add when
    // the impact-analysis plugin is actually built.
    {
        // Periodic background fetch — when toggled on, runs
        // `git fetch` every N minutes (default 5) on the open
        // repo. The interval is editable via QInputDialog when
        // the user toggles ON. Stored as a checkable QAction
        // whose state persists across sessions.
        auto* a = new QAction(menuIcon(QStringLiteral("submodule_update")),
                              tr("&Periodic background fetch"), this);
        a->setObjectName(QStringLiteral("plugins.periodic-background-fetch"));
        a->setCheckable(true);
        const bool savedOn = settingsService_
            ? settingsService_->value(
                "plugins/periodicFetch/enabled", false).toBool()
            : false;
        a->setChecked(savedOn);
        if (savedOn) {
            // Re-arm the timer at startup if the user had it on
            // last time.
            if (!periodicFetchTimer_) {
                periodicFetchTimer_ = new QTimer(this);
                connect(periodicFetchTimer_, &QTimer::timeout, this,
                        [this]() {
                    if (gitService_ && gitService_->isOpen())
                        gitService_->fetch();
                });
            }
            const int mins = settingsService_->value(
                "plugins/periodicFetch/intervalMinutes", 5).toInt();
            periodicFetchTimer_->start(std::max(1, mins) * 60 * 1000);
        }
        // Refresh status bar after construction so the indicator
        // reflects the saved-on state.
        QMetaObject::invokeMethod(this,
            &MainWindow::updatePeriodicFetchStatus,
            Qt::QueuedConnection);
        connect(a, &QAction::toggled, this, [this](bool on) {
            if (on) {
                bool ok = false;
                const int mins = QInputDialog::getInt(
                    this, tr("Periodic Fetch"),
                    tr("Fetch every (minutes):"),
                    /*value=*/ settingsService_->value(
                        "plugins/periodicFetch/intervalMinutes",
                        5).toInt(),
                    /*min=*/1, /*max=*/240, /*step=*/1, &ok);
                if (!ok) return;  // user cancelled — leave checked
                                  // (could also un-check; either is
                                  // defensible)
                if (!periodicFetchTimer_) {
                    periodicFetchTimer_ = new QTimer(this);
                    connect(periodicFetchTimer_, &QTimer::timeout,
                            this, [this]() {
                        if (gitService_ && gitService_->isOpen())
                            gitService_->fetch();
                    });
                }
                periodicFetchTimer_->start(mins * 60 * 1000);
                settingsService_->setValue(
                    "plugins/periodicFetch/enabled", true);
                settingsService_->setValue(
                    "plugins/periodicFetch/intervalMinutes", mins);
                statusBar()->showMessage(
                    tr("Periodic fetch enabled (every %1 min).")
                        .arg(mins), 4000);
            } else {
                if (periodicFetchTimer_) periodicFetchTimer_->stop();
                settingsService_->setValue(
                    "plugins/periodicFetch/enabled", false);
                statusBar()->showMessage(
                    tr("Periodic fetch disabled."), 4000);
            }
            updatePeriodicFetchStatus();
        });
        pluginsMenu->addAction(a);
    }
    {
        // Statistics — quick repo summary: total commits, distinct
        // contributors, top contributor by commit count, branch
        // count, tag count, current HEAD info. Computed in one pass
        // via Repository::createRevWalk so we don't shell out.
        auto* a = new QAction(menuIcon(QStringLiteral("stats")),
                              tr("&Statistics..."), this);
        a->setObjectName(QStringLiteral("plugins.statistics"));
        connect(a, &QAction::triggered, this, [this]() {
            if (!gitService_ || !gitService_->isOpen()) return;

            // One locked pass for the walk + ref counts. The repo
            // lock is held for the whole history walk — background
            // refreshes queue behind it; that's the price of
            // correctness until this moves to a worker thread.
            struct RepoStats {
                bool ok = false;
                QString error;
                int commitCount = 0;
                QHash<QString, int> commitsByAuthor;
                int branchCount = 0;
                int tagCount = 0;
                int remoteCount = 0;
                QString headBranch;
            };
            const RepoStats stats = gitService_->withRepository(
                [](git::Repository& repo) {
                    RepoStats s;
                    auto walkRes = repo.createRevWalk();
                    if (!walkRes.ok()) {
                        s.error = QString::fromStdString(
                            walkRes.error().message());
                        return s;
                    }
                    auto& walk = walkRes.value();
                    walk.setSorting(git::SortOrder::None);
                    walk.pushHead();
                    walk.walk([&](const git::CommitData& c) {
                        ++s.commitCount;
                        s.commitsByAuthor[QString::fromStdString(
                            c.author.name)]++;
                        return true;
                    });
                    if (auto br = repo.branches(git::BranchType::Local);
                        br.ok())
                        s.branchCount =
                            static_cast<int>(br.value().size());
                    if (auto tg = repo.tags(); tg.ok())
                        s.tagCount =
                            static_cast<int>(tg.value().size());
                    if (auto rm = repo.remotes(); rm.ok())
                        s.remoteCount =
                            static_cast<int>(rm.value().size());
                    if (auto hb = repo.headBranchName(); hb.ok())
                        s.headBranch =
                            QString::fromStdString(hb.value());
                    s.ok = true;
                    return s;
                });
            if (!stats.ok) {
                QMessageBox::warning(this, tr("Statistics"),
                    tr("Could not create revwalk: %1").arg(stats.error));
                return;
            }
            const int commitCount = stats.commitCount;
            const QHash<QString, int>& commitsByAuthor =
                stats.commitsByAuthor;
            const int branchCount = stats.branchCount;
            const int tagCount = stats.tagCount;
            const int remoteCount = stats.remoteCount;

            // Top 5 contributors.
            QList<QPair<QString,int>> top;
            for (auto it = commitsByAuthor.cbegin();
                 it != commitsByAuthor.cend(); ++it)
                top.append({it.key(), it.value()});
            std::sort(top.begin(), top.end(),
                [](const auto& a, const auto& b) {
                    return a.second > b.second;
                });
            QStringList topLines;
            const int topN = std::min<int>(5, static_cast<int>(top.size()));
            for (int i = 0; i < topN; ++i)
                topLines << QStringLiteral("  %1 (%2)")
                    .arg(top[i].first).arg(top[i].second);

            const QString headBranch = stats.headBranch;

            const QString text = tr(
                "Repository statistics\n"
                "\n"
                "  Reachable commits from HEAD: %1\n"
                "  Distinct authors:            %2\n"
                "  Local branches:              %3\n"
                "  Remotes:                     %4\n"
                "  Tags:                        %5\n"
                "  Current branch:              %6\n"
                "\n"
                "Top contributors:\n%7"
            ).arg(commitCount)
             .arg(commitsByAuthor.size())
             .arg(branchCount)
             .arg(remoteCount)
             .arg(tagCount)
             .arg(headBranch.isEmpty() ? tr("(detached)") : headBranch)
             .arg(topLines.join('\n'));

            QDialog dlg(this);
            dlg.setWindowTitle(tr("Repository Statistics"));
            dlg.resize(520, 360);
            auto* layout = new QVBoxLayout(&dlg);
            auto* browser = new QPlainTextEdit(&dlg);
            browser->setReadOnly(true);
            browser->setFont(QFontDatabase::systemFont(
                QFontDatabase::FixedFont));
            browser->setPlainText(text);
            layout->addWidget(browser);
            auto* close = new QDialogButtonBox(
                QDialogButtonBox::Close, &dlg);
            connect(close, &QDialogButtonBox::rejected,
                    &dlg, &QDialog::accept);
            layout->addWidget(close);
            dlg.exec();
        });
        pluginsMenu->addAction(a);
    }
    pluginsMenu->addSeparator();
    {
        // Plugin Manager — Git Extensions has a separate dialog,
        // but our settings model puts every per-plugin page inside
        // Tools → Settings. The "Plugin Manager" entry just opens
        // Settings on a sensible page (Plugins). If/when individual
        // plugin pages exist we can add a settings-page enum and
        // open the right one; today there's just the parent page.
        auto* a = new QAction(menuIcon(QStringLiteral("extension")),
                              tr("Plugin &Manager"), this);
        a->setObjectName(QStringLiteral("plugins.plugin-manager"));
        connect(a, &QAction::triggered,
                this, &MainWindow::showSettingsDialog);
        pluginsMenu->addAction(a);
    }
    // "Plugins settings" removed — each plugin already gets its
    // own page under Tools → Settings (following Git Extensions'
    // model), so a separate menu entry duplicates that path.

    // ---- Tools ----
    auto* toolsMenu = menuBar()->addMenu(tr("&Tools"));
    {
        // Git bash → pop a modeless window that hosts a real shell
        // via TerminalWidget. Uses the repo's workdir as the cwd so
        // the user lands in the right directory. No shortcut change
        // from the previous stub (Ctrl+G).
        auto* a = new QAction(menuIcon(QStringLiteral("terminal")),
                              tr("Git &bash"), this);
        a->setObjectName(QStringLiteral("tools.git-bash"));
        a->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_G));
        connect(a, &QAction::triggered, this, [this]() {
            if (!gitService_ || !gitService_->isOpen())
                return;
            auto* dlg = new QDialog(this);
            dlg->setAttribute(Qt::WA_DeleteOnClose);
            dlg->setWindowTitle(tr("Git Bash"));
            dlg->resize(900, 500);
            auto* layout = new QVBoxLayout(dlg);
            layout->setContentsMargins(0, 0, 0, 0);
            auto* term = new widgets::TerminalWidget(dlg);
            layout->addWidget(term);
            term->start(QString::fromStdString(
                gitService_->withRepository(
                    [](git::Repository& r) { return r.workdir(); })));
            dlg->show();
        });
        toolsMenu->addAction(a);
    }
    {
        // GitK → launch the external gitk viewer on PATH, rooted in
        // the current repo's workdir. QProcess::startDetached spawns
        // the child without waiting for it; if gitk isn't installed
        // the call returns false and we surface a friendly message.
        auto* a = new QAction(menuIcon(QStringLiteral("visibility")),
                              tr("Git&K"), this);
        a->setObjectName(QStringLiteral("tools.gitk"));
        connect(a, &QAction::triggered, this, [this]() {
            if (!gitService_ || !gitService_->isOpen()) return;
            const QString workdir = QString::fromStdString(
                gitService_->withRepository(
                    [](git::Repository& r) { return r.workdir(); }));
            const bool ok = QProcess::startDetached(
                QStringLiteral("gitk"), QStringList{}, workdir);
            if (!ok) {
                QMessageBox::warning(this, tr("Launch Failed"),
                    tr("Could not launch gitk. Is it installed and on "
                       "your PATH?"));
            }
        });
        toolsMenu->addAction(a);
    }
    toolsMenu->addSeparator();
    {
        // Git command log → modeless ConsoleOutputWidget that
        // subscribes to GitProcessLog. Every external `git X Y Z`
        // invocation (via Repository::process().run()) shows up as
        // a single line: workdir, command, exit code, duration.
        // The dialog also pre-populates with any commands that
        // already ran since startup (we keep a process-lifetime
        // ring buffer of the last 200 lines).
        auto* a = new QAction(menuIcon(QStringLiteral("command_log")),
                              tr("Git &command log"), this);
        a->setObjectName(QStringLiteral("tools.git-command-log"));
        a->setShortcut(QKeySequence(Qt::Key_F12));
        connect(a, &QAction::triggered, this, [this]() {
            auto* dlg = new QDialog(this);
            dlg->setAttribute(Qt::WA_DeleteOnClose);
            dlg->setWindowTitle(tr("Git Command Log"));
            dlg->resize(900, 420);
            auto* layout = new QVBoxLayout(dlg);
            layout->setContentsMargins(0, 0, 0, 0);
            auto* console = new widgets::ConsoleOutputWidget(dlg);
            layout->addWidget(console);

            // Format helper used by both the historical replay and
            // the live signal — keeps the line shape consistent.
            auto formatLine = [](const QString& workdir,
                                 const QStringList& args,
                                 int exitCode, qint64 ms) -> QString {
                const QString tag =
                    exitCode == 0 ? QStringLiteral("[ok ]")
                                  : exitCode < 0
                                      ? QStringLiteral("[err]")
                                      : QStringLiteral("[%1 ]")
                                            .arg(exitCode, 2);
                return QStringLiteral("%1 %2ms  $ git %3   (in %4)")
                    .arg(tag)
                    .arg(ms, 5)
                    .arg(args.join(' '))
                    .arg(workdir);
            };

            // Replay the ring buffer first so users opening the
            // dialog after a few minutes of activity see the
            // history rather than an empty pane. Live signaling
            // takes over from here.
            const auto past = git::GitProcessLog::instance().recent();
            if (past.empty()) {
                console->appendOutput(tr(
                    "Listening for git commands. Run any operation "
                    "in the main window to see it logged here."));
            } else {
                console->appendOutput(
                    tr("--- %1 prior command(s) replayed below ---")
                        .arg(past.size()));
                for (const auto& e : past) {
                    console->appendOutput(formatLine(
                        e.workdir, e.args, e.exitCode, e.durationMs));
                }
                console->appendOutput(
                    tr("--- live updates follow ---"));
            }

            connect(&git::GitProcessLog::instance(),
                    &git::GitProcessLog::commandLogged,
                    dlg, [console, formatLine](
                        const QString& workdir,
                        const QStringList& args,
                        int exitCode, qint64 ms) {
                console->appendOutput(
                    formatLine(workdir, args, exitCode, ms));
            });

            dlg->show();
        });
        toolsMenu->addAction(a);
    }
    toolsMenu->addSeparator();
    auto* settingsAct = new QAction(menuIcon(QStringLiteral("settings")),
                                    tr("&Settings..."), this);
    settingsAct->setObjectName(QStringLiteral("tools.settings"));
    settingsAct->setShortcut(QKeySequence::Preferences);
    connect(settingsAct, &QAction::triggered,
            this, &MainWindow::showSettingsDialog);
    toolsMenu->addAction(settingsAct);

    // ---- Help ----
    auto* helpMenu = menuBar()->addMenu(tr("&Help"));
    {
        // User manual → GitHub README for now. When we have real
        // online docs (Read the Docs or similar), switch this URL.
        auto* a = new QAction(menuIcon(QStringLiteral("manual")),
                              tr("&User manual"), this);
        a->setObjectName(QStringLiteral("help.user-manual"));
        a->setShortcut(QKeySequence::HelpContents);
        connect(a, &QAction::triggered, this, []() {
            QDesktopServices::openUrl(QUrl(QStringLiteral(
                "https://github.com/ThePieMonster/GitBolt#readme")));
        });
        helpMenu->addAction(a);
    }
    {
        // Changelog → in-app viewer over resources/CHANGELOG.md.
        // Matches Git Extensions' model (bundled file, rendered in
        // a dialog with QTextBrowser's markdown support).
        auto* a = new QAction(menuIcon(QStringLiteral("changelog")),
                              tr("&Changelog"), this);
        a->setObjectName(QStringLiteral("help.changelog"));
        connect(a, &QAction::triggered, this, [this]() {
            QFile f(QStringLiteral(":/content/CHANGELOG.md"));
            QString text;
            if (f.open(QIODevice::ReadOnly | QIODevice::Text))
                text = QString::fromUtf8(f.readAll());
            else
                text = tr("(Could not load CHANGELOG.md from resources.)");

            auto* dlg = new QDialog(this);
            dlg->setAttribute(Qt::WA_DeleteOnClose);
            dlg->setWindowTitle(tr("Changelog"));
            dlg->resize(720, 560);
            auto* layout = new QVBoxLayout(dlg);
            layout->setContentsMargins(0, 0, 0, 0);
            auto* browser = new QTextBrowser(dlg);
            browser->setOpenExternalLinks(true);
            browser->setMarkdown(text);
            layout->addWidget(browser);
            dlg->show();
        });
        helpMenu->addAction(a);
    }
    helpMenu->addSeparator();
    {
        auto* a = new QAction(menuIcon(QStringLiteral("bug")),
                              tr("&Report an issue"), this);
        a->setObjectName(QStringLiteral("help.report-an-issue"));
        connect(a, &QAction::triggered, this, []() {
            QDesktopServices::openUrl(QUrl(QStringLiteral(
                "https://github.com/ThePieMonster/GitBolt/issues/new")));
        });
        helpMenu->addAction(a);
    }
    {
        // "Check for updates" — open the GitHub releases page in
        // the user's browser. A future improvement is an in-app
        // version check that compares the current build against
        // the latest published tag, but the Sparkle-style
        // self-updater that would imply is much bigger scope. The
        // releases page covers the use case ("am I running the
        // latest?") with zero maintenance burden today.
        auto* a = new QAction(menuIcon(QStringLiteral("update")),
                              tr("&Check for updates"), this);
        a->setObjectName(QStringLiteral("help.check-for-updates"));
        connect(a, &QAction::triggered, this, []() {
            QDesktopServices::openUrl(QUrl(QStringLiteral(
                "https://github.com/ThePieMonster/GitBolt/releases")));
        });
        helpMenu->addAction(a);
    }
    helpMenu->addSeparator();
    auto* aboutMenuAct = new QAction(menuIcon(QStringLiteral("about")),
                                     tr("&About GitBolt"), this);
    aboutMenuAct->setObjectName(QStringLiteral("help.about-gitbolt"));
    connect(aboutMenuAct, &QAction::triggered, this, &MainWindow::showAbout);
    helpMenu->addAction(aboutMenuAct);

    // Force icon visibility on every QAction in the menu bar and its
    // submenus. macOS-specific: even with AA_DontShowIconsInMenus set
    // to false, Qt6 sometimes defaults per-action visibility to false.
    std::function<void(QWidget*)> enableIcons = [&](QWidget* w) {
        for (QAction* a : w->actions()) {
            a->setIconVisibleInMenu(true);
            if (a->menu()) enableIcons(a->menu());
        }
    };
    enableIcons(menuBar());

    // Guarantee every menu action is addressable by the test bridge
    // (docs/AGENT_TESTING.md): assign a stable objectName derived
    // from the menu path to any action that doesn't already have an
    // explicit one. New features SHOULD still call setObjectName()
    // themselves — an explicit name survives a display-text change,
    // whereas this fallback is derived from the text — but this
    // ensures nothing is ever untestable, including actions added
    // later by someone who forgets. See assignActionObjectNames().
    assignActionObjectNames(menuBar(), QString());

    // Navigate, View, and Commands only make sense with a repository
    // open. Gray out every child action on the dashboard/home screen
    // so the menus still open but every item is disabled — matching
    // GitExtensions' behavior. We disable the children (not the
    // top-level menu via menuAction()) because Qt's Fusion style on
    // macOS does not visibly dim a disabled menu bar title, but it
    // DOES dim each disabled item in the dropdown. Toggling in
    // onRepositoryOpened() and the Close action keeps this in sync.
    setRepoOnlyMenusEnabled(false);
}

// Walk the Navigate / View / Commands menus and enable or disable
// every child action. Repo-shared QActions (refreshAction_, fetchAction_,
// pullAction_, pushAction_, commitAction_) are toggled independently
// in onRepositoryOpened() and the close-action lambda, so skipping
// them here does not lose correctness — it just avoids a redundant
// write from two places.
void MainWindow::setRepoOnlyMenusEnabled(bool on)
{
    auto toggleChildren = [on](QMenu* m) {
        if (!m) return;
        for (QAction* a : m->actions()) {
            if (a->isSeparator()) continue;
            a->setEnabled(on);
        }
    };
    toggleChildren(navMenu_);
    toggleChildren(viewMenu_);
    toggleChildren(cmdMenu_);
    toggleChildren(repoMenu_);
}

void MainWindow::setRepoActionsEnabled(bool on)
{
    // Each action is shared between the Repository menu and the
    // toolbar, so one setEnabled() toggles both entry points (see
    // MainWindow.h). Used by onRepositoryOpened (true), the Close
    // action (false), the optimistic phase of an async open (false
    // — ops would hit the previous repo), and the failure revert.
    if (refreshAction_) refreshAction_->setEnabled(on);
    if (fetchAction_)   fetchAction_->setEnabled(on);
    if (pullAction_)    pullAction_->setEnabled(on);
    if (pushAction_)    pushAction_->setEnabled(on);
    if (commitAction_)  commitAction_->setEnabled(on);
    if (filterInput_)   filterInput_->setEnabled(on);
    if (branchCombo_)   branchCombo_->setEnabled(on);
}

void MainWindow::assignActionObjectNames(QWidget* widget,
                                         const QString& pathPrefix)
{
    // Display-text → slug, mirroring TestBridge::slugify so the
    // auto-assigned objectName lines up with the menu-path the
    // bridge derives ("commands.resolve-conflicts"). Strips
    // mnemonics and ellipses, lowercases, collapses runs of
    // non-alphanumerics to single hyphens.
    auto slug = [](QString text) {
        text.remove(QLatin1Char('&'));
        text.remove(QStringLiteral("..."));
        text.remove(QStringLiteral("…"));
        text = text.toLower();
        text.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")),
                     QStringLiteral("-"));
        while (text.startsWith(QLatin1Char('-'))) text.remove(0, 1);
        while (text.endsWith(QLatin1Char('-')))   text.chop(1);
        return text;
    };

    for (QAction* a : widget->actions()) {
        if (a->isSeparator())
            continue;
        if (QMenu* sub = a->menu()) {
            // Recurse into the submenu, extending the path with this
            // menu's title. The top-level menu titles ("File",
            // "Commands", …) form the first path segment.
            const QString seg = slug(sub->title());
            assignActionObjectNames(
                sub, pathPrefix.isEmpty() ? seg
                                          : pathPrefix + QLatin1Char('.') + seg);
            continue;
        }
        // Leaf action. Respect an explicit objectName if the call
        // site already set one (preferred — stable across text
        // changes); only fill in the gaps.
        if (a->objectName().isEmpty()) {
            const QString name = slug(a->text());
            if (!name.isEmpty())
                a->setObjectName(pathPrefix.isEmpty()
                                     ? name
                                     : pathPrefix + QLatin1Char('.') + name);
        }
    }
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

    // All repo-dependent actions reuse the QAction instances
    // already constructed in createMenuBar() — addAction(QAction*)
    // shares one action between the menu and the toolbar, so a
    // single setEnabled() call toggles both. This is why the menu
    // MUST be created before the toolbar (see the ordering in the
    // MainWindow constructor).
    toolbar->addAction(refreshAction_);

    toolbar->addSeparator();

    // Branch quick-switch combo. Sits before fetch/pull/push because
    // those operations all key off "the current branch" — having the
    // current branch readout right next to them mirrors the visual
    // grouping in Git Extensions' toolbar (a small branch icon +
    // dropdown sits left of the sync buttons).
    {
        auto* branchLabel = new QLabel(tr("Branch:"), toolbar);
        branchLabel->setContentsMargins(4, 0, 4, 0);
        branchLabelAction_ = toolbar->addWidget(branchLabel);

        branchCombo_ = new QComboBox(toolbar);
        branchCombo_->setMinimumWidth(180);
        branchCombo_->setMaximumWidth(280);
        branchCombo_->setEnabled(false);
        branchCombo_->setToolTip(
            tr("Switch the working tree to another local branch."));
        // `activated` only fires for user-driven changes, not the
        // programmatic setCurrentIndex() we use during populate.
        // That's exactly what we want — populating must NOT
        // trigger a checkout. Use the int overload (the QString
        // overload was deprecated in Qt6).
        connect(branchCombo_,
                qOverload<int>(&QComboBox::activated),
                this, [this](int idx) {
            if (!gitService_ || !gitService_->isOpen()) return;
            if (idx < 0) return;
            const QString name = branchCombo_->itemText(idx);
            if (name.isEmpty()) return;
            // Cheap guard against re-checking out the current branch
            // (no-op but still triggers refreshes). The combo is
            // populated with the current branch pre-selected, and
            // the user picking the same row would emit `activated`.
            const QString currentName = branchCombo_->property(
                "currentBranch").toString();
            if (name == currentName) return;

            // Show inline feedback that the click registered. The
            // checkout itself is fast, but the follow-up refreshes
            // (status, log, branches) take a moment, and clicking
            // a branch name with no visible reaction was the same
            // problem fetch/pull/push had. The label gets cleared
            // automatically by the next branchesReady (which the
            // checkout triggers via refreshBranches).
            if (remoteOpClearTimer_ && remoteOpClearTimer_->isActive())
                remoteOpClearTimer_->stop();
            if (remoteOpLabel_) {
                remoteOpLabel_->setStyleSheet(QStringLiteral(
                    "QLabel { color: palette(window-text); font-style: italic; }"));
                remoteOpLabel_->setText(tr("Switching to %1…").arg(name));
            }
            statusBar()->showMessage(
                tr("Switching to %1…").arg(name), 2000);

            // Reset the failure flag before invoking; if the checkout
            // emits operationFailed (e.g. dirty working tree conflicts
            // with the target branch's main.cpp), the handler we
            // registered in setupConnections will flip the flag to
            // true synchronously, before checkoutBranch returns. We
            // check it on the way out and choose green ✓ vs red ✗
            // accordingly. Without this, a failed checkout used to
            // wrongly show "✓ Switched to feature/work" while the
            // current branch was unchanged.
            lastRemoteOpFailed_ = false;
            gitService_->checkoutBranch(name);

            if (remoteOpLabel_) {
                if (lastRemoteOpFailed_) {
                    QString status = statusBar()->currentMessage();
                    QString oneLine = status;
                    oneLine.replace(QChar('\n'), QStringLiteral(" | "));
                    oneLine.replace(QChar('\r'), QString());
                    remoteOpLabel_->setStyleSheet(QStringLiteral(
                        "QLabel { color: #c43c3c; font-weight: bold; }"));
                    remoteOpLabel_->setText(tr("✗ %1").arg(oneLine));
                    remoteOpLabel_->setToolTip(status);
                } else {
                    remoteOpLabel_->setStyleSheet(QStringLiteral(
                        "QLabel { color: #2e9c36; font-weight: bold; }"));
                    remoteOpLabel_->setText(tr("✓ Switched to %1").arg(name));
                    remoteOpLabel_->setToolTip(QString());
                }
            }
            if (!remoteOpClearTimer_) {
                remoteOpClearTimer_ = new QTimer(this);
                remoteOpClearTimer_->setSingleShot(true);
                connect(remoteOpClearTimer_, &QTimer::timeout, this, [this]() {
                    if (remoteOpLabel_) {
                        remoteOpLabel_->clear();
                        remoteOpLabel_->setStyleSheet(QString());
                    }
                });
            }
            remoteOpClearTimer_->start(lastRemoteOpFailed_ ? 6000 : 2500);
        });
        branchComboAction_ = toolbar->addWidget(branchCombo_);
    }

    toolbar->addSeparator();

    toolbar->addAction(fetchAction_);
    toolbar->addAction(pullAction_);
    toolbar->addAction(pushAction_);

    toolbar->addSeparator();

    toolbar->addAction(commitAction_);

    // Force the Commit button alone to show its text BESIDE the
    // icon — the rest of the toolbar uses Qt's default icon-only
    // style. Commit is special because we live-update its text
    // with the changed-file count ("Commit (4)..."), and that
    // count is invisible if the button is icon-only. Matches
    // Git Extensions' toolbar where Commit is the one labeled
    // button while everything else is iconified.
    //
    // Match the labeled toolbar buttons (Commit and Stash) to the
    // QLabel font used by the "Branch:" / "Filter:" labels in this
    // same toolbar. Qt6 on macOS picks a smaller default font for
    // QToolButton than QLabel, which makes the only two text-bearing
    // buttons read as visually demoted next to the inline labels.
    // Using the application default font puts every text element on
    // the toolbar at one consistent size.
    const QFont labeledToolButtonFont = QApplication::font();
    auto matchToolButtonFont = [&](QToolButton* btn) {
        if (btn) btn->setFont(labeledToolButtonFont);
    };

    if (auto* btn = qobject_cast<QToolButton*>(
            toolbar->widgetForAction(commitAction_))) {
        btn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        matchToolButtonFont(btn);
    }

    // Stash button shares its QAction with the Commands menu's
    // "Manage stashes…" entry, so a single setEnabled() call (handled
    // by setRepoOnlyMenusEnabled when a repo opens / closes) toggles
    // both. Show it text-beside-icon like Commit so the label "Stash"
    // is visible on the toolbar.
    if (stashAction_) {
        toolbar->addAction(stashAction_);
        if (auto* btn = qobject_cast<QToolButton*>(
                toolbar->widgetForAction(stashAction_))) {
            btn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            matchToolButtonFont(btn);
        }
    }

    // Settings used to live here as a toolbar button too, but it
    // duplicated Tools → Settings. The menu entry is the canonical
    // home; keeping two entry points to the same dialog on screen
    // made the toolbar feel cluttered.

    // Inline activity indicator. Sits in the empty middle area between
    // the labeled buttons (Commit/Stash) and the right-aligned Filter,
    // so it doesn't push Commit/Stash off to the right when collapsed.
    // Empty by default; runRemoteOp() flips it to "Fetching from origin…"
    // while the op runs and "✓ Fetch complete" (or red "✗ failed")
    // afterwards.
    remoteOpLabel_ = new QLabel(toolbar);
    remoteOpLabel_->setContentsMargins(12, 0, 12, 0);
    remoteOpLabel_->setText(QString());
    remoteOpLabelAction_ = toolbar->addWidget(remoteOpLabel_);
    remoteOpLabelAction_->setVisible(false);  // hidden until repo opens

    // Right-aligned spacer so the filter sits at the far end like
    // GitExtensions' "Filter:" input.
    auto* spacer = new QWidget(toolbar);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbar->addWidget(spacer);

    auto* filterLabel = new QLabel(tr("Filter:"), toolbar);
    filterLabel->setContentsMargins(4, 0, 4, 0);
    filterLabelAction_ = toolbar->addWidget(filterLabel);

    filterInput_ = new QLineEdit(toolbar);
    filterInput_->setPlaceholderText(tr("Search commits…"));
    filterInput_->setClearButtonEnabled(true);
    filterInput_->setMaximumWidth(220);
    filterInput_->setEnabled(false);
    filterInput_->setToolTip(
        tr("Filter the revision grid by commit message "
           "(case-insensitive substring)."));
    // Live filtering: every keystroke updates the proxy. Cheap
    // because the source model is bounded (default 256 commits)
    // and QSortFilterProxyModel does string filtering in O(rows).
    connect(filterInput_, &QLineEdit::textChanged,
            this, [this](const QString& text) {
        if (repoView_ && repoView_->revisionGraph())
            repoView_->revisionGraph()->setFilterText(text);
    });
    filterInputAction_ = toolbar->addWidget(filterInput_);

    // Hide the filter pair on the home screen — it only makes
    // sense once a repo is open and there's a revision grid to
    // filter. onRepositoryOpened() flips these back to visible,
    // and the Close action flips them off again. Same goes for
    // the branch quick-switch combo.
    if (filterLabelAction_) filterLabelAction_->setVisible(false);
    if (filterInputAction_) filterInputAction_->setVisible(false);
    if (branchLabelAction_) branchLabelAction_->setVisible(false);
    if (branchComboAction_) branchComboAction_->setVisible(false);

    // Name any toolbar-only actions (most are shared QAction
    // instances already named via the menu walk; this covers the
    // rest under a "toolbar." path). Runs after createMenuBar so
    // shared actions keep their menu-derived names.
    assignActionObjectNames(toolbar, QStringLiteral("toolbar"));
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

    // Permanent right-side indicator for the Periodic fetch
    // plugin. Empty when the feature is off; shows
    // "🔄 every Nm" when enabled. Permanent widgets sit to the
    // right of any showMessage() text and don't get clobbered
    // by transient status updates.
    periodicFetchStatus_ = new QLabel(this);
    periodicFetchStatus_->setContentsMargins(8, 0, 8, 0);
    statusBar()->addPermanentWidget(periodicFetchStatus_);

    statusBar()->showMessage(tr("Ready"));
}

void MainWindow::updatePeriodicFetchStatus()
{
    if (!periodicFetchStatus_) return;
    if (periodicFetchTimer_ && periodicFetchTimer_->isActive()) {
        const int mins = periodicFetchTimer_->interval() / 60000;
        periodicFetchStatus_->setText(
            tr("Auto-fetch: every %1 min").arg(mins));
    } else {
        periodicFetchStatus_->clear();
    }
}

// ---------------------------------------------------------------------------
// Signal/slot connections
// ---------------------------------------------------------------------------
void MainWindow::setupConnections()
{
    // --- GitService signals ---
    connect(gitService_, &services::GitService::repositoryOpened,
            this, &MainWindow::onRepositoryOpened);

    connect(gitService_, &services::GitService::repositoryOpenFailed,
            this, &MainWindow::onRepositoryOpenFailed);

    // Cherry-pick outcome: success gets a status-bar note; a
    // conflict stop offers the three-way resolver (libgit2 leaves
    // the repository in CHERRYPICK state with conflicted index
    // entries, which is exactly what the resolver reads).
    connect(gitService_, &services::GitService::cherryPickComplete,
            this, [this](bool success, const QString& message) {
        statusBar()->showMessage(message, 4000);
        if (!success)
            offerConflictResolution(tr("cherry-pick"));
    });

    connect(gitService_, &services::GitService::logReady,
            this, &MainWindow::onLogReady);

    connect(gitService_, &services::GitService::branchesReady,
            this, &MainWindow::onBranchesReady);

    // Tags feed the sidebar's Tags category. Without this connect
    // (and the refreshTags() call in onRepositoryOpened) the
    // category sat permanently empty — refreshTags results were
    // emitted to nobody.
    connect(gitService_, &services::GitService::tagsReady,
            this, [this](std::vector<git::TagInfo> tags) {
        if (repoView_)
            repoView_->setTags(std::move(tags));
    });

    // Live count for the toolbar's Commit button — shows how many
    // working-tree files have changes the user could commit. We
    // count any entry whose status isn't Current/Ignored, which
    // matches the "files in the staging widget" definition: tracked
    // modifications/deletions/renames AND untracked files (the
    // Commit dialog can stage them too).
    connect(gitService_, &services::GitService::statusReady,
            this, [this](std::vector<git::StatusEntry> entries) {
        if (!commitAction_) return;
        int n = 0;
        for (const auto& e : entries) {
            if (git::hasFlag(e.status, git::FileStatus::Ignored))
                continue;
            if (e.status == git::FileStatus::Current) continue;
            ++n;
        }
        // Keep the mnemonic on the C of "Commit". The trailing
        // "..." stays since this still opens a dialog.
        if (n > 0)
            commitAction_->setText(tr("Co&mmit (%1)...").arg(n));
        else
            commitAction_->setText(tr("Co&mmit..."));
    });

    connect(gitService_, &services::GitService::submodulesReady,
            this, [this](std::vector<git::SubmoduleInfo> subs) {
                if (repoView_) repoView_->setSubmodules(std::move(subs));
            });

    connect(gitService_, &services::GitService::stashesReady,
            this, [this](std::vector<git::StashEntry> stashes) {
                if (repoView_) repoView_->setStashes(std::move(stashes));
            });

    // Status-bar only surfacing for git op results. The Console
    // tab is an interactive terminal now — it's not the right
    // place to dump passive log messages.
    connect(gitService_, &services::GitService::operationFailed,
            this, [this](const QString& op, const QString& err) {
                statusBar()->showMessage(op + tr(" failed: ") + err, 5000);
                // Tell the fetch/pull/push wrappers to skip their
                // own success-confirmation message — the failure
                // text we just put on the status bar should stay.
                if (op.startsWith(QLatin1String("fetch"),    Qt::CaseInsensitive) ||
                    op.startsWith(QLatin1String("pull"),     Qt::CaseInsensitive) ||
                    op.startsWith(QLatin1String("push"),     Qt::CaseInsensitive) ||
                    op.startsWith(QLatin1String("checkout"), Qt::CaseInsensitive)) {
                    lastRemoteOpFailed_ = true;
                }
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

    // --- Commit selection → back/forward history ---
    //
    // Every selection change in the revision graph feeds the
    // navigation stack. The suppress flag is set by Back/Forward
    // before they programmatically reselect, so those paths don't
    // pollute their own stacks. Without it pressing Back would push
    // the destination commit onto backHistory_, defeating the point.
    if (repoView_ && repoView_->revisionGraph()) {
        connect(repoView_->revisionGraph(),
                &widgets::RevisionGraphWidget::commitSelected,
                this, [this](const QString& hash) {
            if (suppressHistoryPush_) {
                suppressHistoryPush_ = false;
                currentNavCommit_ = hash;
                return;
            }
            pushHistory(hash);
        });
    }

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

    // --- Branch tree actions (BranchTreeWidget lives inside RepositoryView).
    // Every context-menu entry and the sidebar's "New Branch" button
    // routes through these. createBranch/deleteBranch go through
    // GitService (which emits operationFailed on error); rename /
    // merge / set-upstream have no libgit2 wrapper yet so they run
    // through the git CLI with the same handleProcessResult feedback
    // the Commands-menu equivalents use.
    if (auto* branchTree = repoView_->branchTree()) {
        connect(branchTree, &widgets::BranchTreeWidget::checkoutRequested,
                gitService_, &services::GitService::checkoutBranch);
        connect(branchTree, &widgets::BranchTreeWidget::createBranchRequested,
                gitService_, &services::GitService::createBranch);
        connect(branchTree, &widgets::BranchTreeWidget::deleteBranchRequested,
                gitService_, &services::GitService::deleteBranch);
        connect(branchTree, &widgets::BranchTreeWidget::pushRequested,
                gitService_, &services::GitService::push);
        connect(branchTree, &widgets::BranchTreeWidget::renameBranchRequested,
                this, [this](const QString& oldName, const QString& newName) {
            if (!gitService_ || !gitService_->isOpen()) return;
            auto out = gitService_->process().run(
                {"branch", "-m", oldName.toStdString(),
                 newName.toStdString()});
            handleProcessResult(this, tr("Rename Branch Failed"), out);
            gitService_->refreshBranches();
        });
        connect(branchTree, &widgets::BranchTreeWidget::mergeRequested,
                this, [this](const QString& name) {
            if (!gitService_ || !gitService_->isOpen()) return;
            auto out = gitService_->process().run(
                {"merge", name.toStdString()});
            const bool merged =
                handleProcessResult(this, tr("Merge Failed"), out);
            gitService_->refreshStatus();
            gitService_->refreshLog();
            gitService_->refreshBranches();
            if (!merged)
                offerConflictResolution(tr("merge"));
        });
        connect(branchTree, &widgets::BranchTreeWidget::setUpstreamRequested,
                this, [this](const QString& branch, const QString& upstream) {
            if (!gitService_ || !gitService_->isOpen()) return;
            auto out = gitService_->process().run(
                {"branch",
                 "--set-upstream-to=" + upstream.toStdString(),
                 branch.toStdString()});
            handleProcessResult(this, tr("Set Upstream Failed"), out);
            gitService_->refreshBranches();
        });
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

    // Optimistic open: switch to the repository view immediately
    // with a busy overlay and let the open + data loads stream in
    // on worker threads. Previously this called the synchronous
    // GitService::openRepository, which froze the app on whatever
    // screen was active — up to ~a minute on large repos (libgit2
    // open + a recursive working-tree walk for the file watcher).

    // Snapshot where the user was so a failed open can revert.
    // Only the first open of a burst snapshots; if another open is
    // already pending, the current widget/title ARE the loading
    // screen, which would be a useless revert target.
    if (pendingOpenPath_.isEmpty()) {
        widgetBeforeOpen_ = centralStack_->currentWidget();
        titleBeforeOpen_  = windowTitle();
    }
    pendingOpenPath_   = path;
    awaitingInitialLog_ = true;

    const QString repoName = QDir(path).dirName();
    setWindowTitle(tr("Opening %1…").arg(repoName.isEmpty() ? path : repoName));
    centralStack_->setCurrentWidget(repoView_);

    // Clear every model/pane that still shows the previous repo —
    // the user must never see repo A's data under repo B's title.
    if (commitLogModel_)
        commitLogModel_->clear();
    if (repoView_) {
        repoView_->setBranches({});
        repoView_->setTags({});
        repoView_->setStashes({});
        repoView_->setSubmodules({});
        repoView_->resetInspectorTabs();
    }
    if (filterInput_)
        filterInput_->clear();
    if (branchCombo_) {
        branchCombo_->clear();
        branchCombo_->setProperty("currentBranch", QString());
    }
    branchLabel_->setText(tr("Opening…"));
    repoPathLabel_->setText(path);

    // Block repo-dependent operations until the open lands — a
    // Pull/Commit fired now would hit the previous repository.
    setRepoActionsEnabled(false);

    if (repoView_)
        repoView_->showLoading(tr("Opening repository…"));
    statusBar()->showMessage(tr("Opening %1…").arg(path));

    gitService_->openRepositoryAsync(path);
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
    // Async open landed (or a sync open from another path
    // completed) — no longer pending. The loading overlay stays up
    // until the first commit-log page arrives in onLogReady.
    pendingOpenPath_.clear();

    // Lead with the repo name (the working-tree directory's
    // basename) instead of the app name. "GitBolt - /path" was
    // redundant: the app name lives in the macOS Apple menu and
    // the dock already, so the title bar is more useful when it
    // names the document being worked on. Two spaces around the
    // dash give the repo name and path visible breathing room.
    // QDir::dirName handles trailing slashes; fall back to the
    // app name if for any reason the path is empty.
    const QString repoName = QDir(path).dirName();
    setWindowTitle(
        (repoName.isEmpty() ? QStringLiteral("GitBolt") : repoName) +
        QStringLiteral("  -  ") + path);
    centralStack_->setCurrentWidget(repoView_);
    settingsService_->addRecentRepository(path);
    updateRecentMenu();

    // Clear stale state from the previous repo before loading the new one.
    if (filterInput_)
        filterInput_->clear();
    if (repoView_) {
        repoView_->resetInspectorTabs();
        repoView_->setRepositoryPath(path);

        // Re-apply persisted View → column visibility toggles.
        // The actions were restored from QSettings at construction
        // time but the graph view didn't have a model then; now it
        // does, so we push the saved state through.
        if (auto* graph = repoView_->revisionGraph()) {
            for (const auto& ct : columnToggles_) {
                if (ct.action)
                    graph->setColumnVisible(
                        ct.columnIndex, ct.action->isChecked());
            }
        }
        // Same for branch-tree section visibility toggles.
        if (auto* tree = repoView_->branchTree()) {
            for (const auto& bt : branchTreeToggles_) {
                if (bt.action)
                    tree->setCategoryVisible(
                        bt.categoryIndex, bt.action->isChecked());
            }
        }
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
    setRepoActionsEnabled(true);

    // Reveal the filter pair now that there's actually a
    // revision grid for it to filter, and the branch quick-switch
    // dropdown.
    if (filterLabelAction_) filterLabelAction_->setVisible(true);
    if (filterInputAction_) filterInputAction_->setVisible(true);
    if (branchLabelAction_) branchLabelAction_->setVisible(true);
    if (branchComboAction_) branchComboAction_->setVisible(true);
    if (remoteOpLabelAction_) remoteOpLabelAction_->setVisible(true);

    // Light up every item under Navigate / View / Commands.
    setRepoOnlyMenusEnabled(true);

    repoPathLabel_->setText(path);

    const QString headBranch = gitService_->withRepository(
        [](git::Repository& r) -> QString {
            auto branchResult = r.headBranchName();
            return branchResult
                ? QString::fromStdString(*branchResult)
                : QString();
        });
    branchLabel_->setText(headBranch.isEmpty()
        ? tr("HEAD (detached)")
        : tr("Branch: %1").arg(headBranch));

    // Refresh the remaining sidebar categories.
    gitService_->refreshStashes();
    gitService_->refreshSubmodules();
    gitService_->refreshTags();

    statusBar()->showMessage(tr("Opened: %1").arg(path), 3000);
}

void MainWindow::onLogReady(std::vector<gitbolt::git::CommitData> commits, int offset)
{
    if (offset == 0) {
        commitLogModel_->setCommits(std::move(commits));
    } else {
        commitLogModel_->appendCommits(commits);
    }

    // First page after an async open: the main area now has real
    // content (or a guaranteed-empty page for unborn-HEAD repos —
    // refreshLog always emits for offset 0), so drop the loading
    // overlay. Stale logReady from a previous repo can't get here:
    // GitService discards results whose repository was swapped out.
    if (offset == 0 && awaitingInitialLog_) {
        awaitingInitialLog_ = false;
        if (repoView_)
            repoView_->hideLoading();
    }
}

void MainWindow::onRepositoryOpenFailed(const QString& path, const QString& error)
{
    pendingOpenPath_.clear();
    awaitingInitialLog_ = false;
    if (repoView_)
        repoView_->hideLoading();

    if (gitService_->isOpen()) {
        // The failed open never touched the previously loaded
        // repository — put the user back where they were and
        // repopulate the models we cleared optimistically. The
        // refreshes are async and cheap for an already-open repo.
        setWindowTitle(titleBeforeOpen_);
        centralStack_->setCurrentWidget(
            widgetBeforeOpen_ ? widgetBeforeOpen_
                              : static_cast<QWidget*>(dashboardView_));
        setRepoActionsEnabled(true);

        struct HeadInfo {
            bool open = false;
            QString workdir;
            QString branch;
        };
        const HeadInfo info = gitService_->withRepository(
            [](git::Repository& r) {
                HeadInfo h;
                h.open = true;
                h.workdir = QString::fromStdString(r.workdir());
                if (auto branchResult = r.headBranchName(); branchResult)
                    h.branch = QString::fromStdString(*branchResult);
                return h;
            });
        if (info.open) {
            repoPathLabel_->setText(info.workdir);
            branchLabel_->setText(info.branch.isEmpty()
                ? tr("HEAD (detached)")
                : tr("Branch: %1").arg(info.branch));
        }
        gitService_->refreshStatus();
        gitService_->refreshLog();
        gitService_->refreshBranches();
        gitService_->refreshStashes();
        gitService_->refreshSubmodules();
        gitService_->refreshTags();
    } else {
        // Nothing usable to fall back to — home screen.
        setWindowTitle(QStringLiteral("GitBolt"));
        centralStack_->setCurrentWidget(dashboardView_);
        branchLabel_->setText(tr("No repository"));
        repoPathLabel_->setText(QString());
    }

    statusBar()->showMessage(tr("Failed to open %1").arg(path), 4000);
    QMessageBox::warning(this, tr("Error"),
        tr("Failed to open repository at %1\n\n%2").arg(path, error));
}

void MainWindow::onBranchesReady(std::vector<gitbolt::git::BranchInfo> branches)
{
    // Build the toolbar combo's contents from the local-branch
    // subset BEFORE moving the vector into repoView_. Block signals
    // so the populate doesn't fire `activated` (we want activated
    // to mean "user picked a branch", not "we refreshed the list").
    if (branchCombo_) {
        const QSignalBlocker blocker(branchCombo_);
        branchCombo_->clear();
        QString currentBranch;
        for (const auto& b : branches) {
            if (b.type != git::BranchType::Local) continue;
            const QString name = QString::fromStdString(b.name);
            branchCombo_->addItem(name);
            if (b.isHead) currentBranch = name;
        }
        branchCombo_->setProperty("currentBranch", currentBranch);
        if (!currentBranch.isEmpty()) {
            const int idx = branchCombo_->findText(currentBranch);
            if (idx >= 0) branchCombo_->setCurrentIndex(idx);
        }
    }

    // Forward branches to the repository view (which owns the
    // BranchTreeWidget inside its left pane).
    if (repoView_)
        repoView_->setBranches(std::move(branches));

    // Also update the branch label in the status bar.
    const QString headBranch = gitService_->withRepository(
        [](git::Repository& r) -> QString {
            auto branchResult = r.headBranchName();
            return branchResult
                ? QString::fromStdString(*branchResult)
                : QString();
        });
    if (!headBranch.isEmpty())
        branchLabel_->setText(tr("Branch: %1").arg(headBranch));
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

    int idx = 0;
    for (const QString& path : recent) {
        QAction* a = recentMenu_->addAction(path, this, [this, path]() {
            // Same async path as File → Open: immediate repo view
            // with loading overlay; failure handling lives in
            // onRepositoryOpenFailed.
            openRepositoryAtPath(path);
        });
        // This submenu is rebuilt on every change, AFTER the
        // createMenuBar auto-namer ran, so name the entries here.
        // By index, not path, so the names are stable as the list
        // reorders; the bridge can read each entry's path from its
        // list-actions "text" field.
        a->setObjectName(QStringLiteral("file.recent.%1").arg(idx++));
    }
}

void MainWindow::pushHistory(const QString& commitHash)
{
    // No-op for the very first selection (no "previous" to push)
    // and for re-selecting the same commit (clicking the same row
    // twice shouldn't grow the history).
    if (commitHash.isEmpty()) return;
    if (currentNavCommit_ != commitHash) {
        if (!currentNavCommit_.isEmpty())
            backHistory_.push_back(currentNavCommit_);
        // A genuine new navigation invalidates any forward path
        // (same model browser back/forward use).
        forwardHistory_.clear();
    }
    currentNavCommit_ = commitHash;
}

// ---------------------------------------------------------------------------
// Conflict resolution
// ---------------------------------------------------------------------------
void MainWindow::showConflictResolver()
{
    if (!gitService_ || !gitService_->isOpen())
        return;

    auto* dlg = new QDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle(tr("Resolve Conflicts"));
    dlg->setObjectName(QStringLiteral("dlg.conflicts"));
    conf::SettingsService::applyConfiguredSize(dlg, "conflicts");

    auto* layout = new QVBoxLayout(dlg);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* widget = new widgets::MergeConflictWidget(dlg);
    layout->addWidget(widget);

    // One-shot population on the first conflictsReady. A standing
    // connection would re-populate on every later conflict-list
    // change and stomp the user's in-progress resolutions.
    auto conn = std::make_shared<QMetaObject::Connection>();
    *conn = connect(
        gitService_, &services::GitService::conflictsReady,
        dlg, [this, dlg, widget, conn](
                 std::vector<git::MergeConflictEntry> conflicts) {
            QObject::disconnect(*conn);
            if (conflicts.empty()) {
                QMessageBox::information(
                    this, tr("Resolve Conflicts"),
                    tr("No conflicted files — nothing to resolve."));
                dlg->close();
                return;
            }
            widget->setConflicts(conflicts);
        });
    gitService_->refreshConflicts();

    connect(widget,
            &widgets::MergeConflictWidget::allConflictsResolved,
            dlg, [this, dlg, widget]() {
        const auto& conflicts = widget->conflicts();
        const auto contents = widget->allResolvedContents();
        std::vector<std::pair<QString, QString>> resolutions;
        resolutions.reserve(conflicts.size());
        for (size_t i = 0;
             i < conflicts.size() && i < contents.size(); ++i) {
            resolutions.emplace_back(
                QString::fromStdString(conflicts[i].path),
                contents[i]);
        }
        gitService_->resolveConflicts(resolutions);
        statusBar()->showMessage(
            tr("Conflicts resolved and staged — commit to conclude "
               "the operation."), 6000);
        dlg->close();
    });

    connect(widget, &widgets::MergeConflictWidget::mergeAborted,
            dlg, [this, dlg]() {
        // The widget has already confirmed with the user.
        gitService_->abortConflictState();
        dlg->close();
    });

    dlg->show();
}

void MainWindow::offerConflictResolution(const QString& operation)
{
    if (!gitService_ || !gitService_->isOpen())
        return;

    // Only offer when the repo is actually mid-operation — a merge
    // can fail for plenty of non-conflict reasons (dirty tree,
    // unknown ref) where the resolver would have nothing to show.
    const bool inProgress = gitService_->withRepository(
        [](git::Repository& r) {
            return r.state() != git::RepoState::None;
        });
    if (!inProgress)
        return;

    const auto answer = QMessageBox::question(
        this, tr("Conflicts"),
        tr("The %1 stopped on conflicts.\n\nOpen the conflict "
           "resolver now?").arg(operation));
    if (answer == QMessageBox::Yes)
        showConflictResolver();
}

} // namespace gitbolt::ui
