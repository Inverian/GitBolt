#include "ui/MainWindow.h"
#include "ui/RepositoryView.h"
#include "ui/DashboardView.h"
#include "models/CommitLogModel.h"
#include "services/GitService.h"
#include "conf/SettingsService.h"
#include "widgets/BranchTreeWidget.h"
#include "widgets/StagingWidget.h"
#include "widgets/CommitEditorWidget.h"
#include "widgets/ConsoleOutputWidget.h"
#include "widgets/RevisionGraphWidget.h"

#include <QApplication>
#include <QDockWidget>
#include <QFileDialog>
#include <QLabel>
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

    centralStack_->addWidget(dashboardView_);
    centralStack_->addWidget(repoView_);
    setCentralWidget(centralStack_);

    createMenuBar();
    createToolBar();
    createStatusBar();
    createDockWidgets();
    setupConnections();
}

MainWindow::~MainWindow() = default;

// ---------------------------------------------------------------------------
// Menu bar
// ---------------------------------------------------------------------------
void MainWindow::createMenuBar()
{
    // ---- File ----
    auto* fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->addAction(tr("&Open Repository..."), this,
                        &MainWindow::openRepository, QKeySequence::Open);

    recentMenu_ = fileMenu->addMenu(tr("Recent Repositories"));
    updateRecentMenu();

    fileMenu->addSeparator();
    fileMenu->addAction(tr("&Quit"), qApp, &QApplication::quit, QKeySequence::Quit);

    // ---- Repository ----
    auto* repoMenu = menuBar()->addMenu(tr("&Repository"));
    repoMenu->addAction(tr("&Refresh"), this, [this]() {
        gitService_->refreshStatus();
        gitService_->refreshLog();
        gitService_->refreshBranches();
    }, QKeySequence::Refresh);
    repoMenu->addSeparator();
    repoMenu->addAction(tr("&Fetch"), this, [this]() { gitService_->fetch(); });
    repoMenu->addAction(tr("Pu&ll"), this, [this]() { gitService_->pull("origin", ""); });
    repoMenu->addAction(tr("&Push"), this, [this]() { gitService_->push("origin", ""); });
    repoMenu->addSeparator();
    repoMenu->addAction(tr("&Commit"), this, [this]() {
        if (commitEditorWidget_) {
            QString msg = commitEditorWidget_->message();
            if (!msg.isEmpty()) {
                gitService_->commitChanges(msg);
                commitEditorWidget_->clear();
            }
        }
    });

    // ---- Branch ----
    auto* branchMenu = menuBar()->addMenu(tr("&Branch"));
    branchMenu->addAction(tr("&New Branch..."), this, [this]() {
        // Placeholder: in a full impl this would show an input dialog
        gitService_->createBranch(QStringLiteral("new-branch"));
    });
    branchMenu->addAction(tr("&Delete Branch..."), this, [this]() {
        gitService_->deleteBranch(QStringLiteral(""));
    });
    branchMenu->addAction(tr("&Checkout..."), this, [this]() {
        gitService_->checkoutBranch(QStringLiteral(""));
    });
    branchMenu->addAction(tr("&Merge..."), this, []() {
        // Placeholder for merge dialog
    });

    // ---- Help ----
    auto* helpMenu = menuBar()->addMenu(tr("&Help"));
    helpMenu->addAction(tr("&About GitBolt"), this, &MainWindow::showAbout);
}

// ---------------------------------------------------------------------------
// Toolbar
// ---------------------------------------------------------------------------
void MainWindow::createToolBar()
{
    auto* toolbar = addToolBar(tr("Main"));
    toolbar->setMovable(false);
    toolbar->setIconSize(QSize(18, 18));

    toolbar->addAction(tr("Open"), this, &MainWindow::openRepository);
    toolbar->addSeparator();
    toolbar->addAction(tr("Fetch"),  this, [this]() { gitService_->fetch(); });
    toolbar->addAction(tr("Pull"),   this, [this]() { gitService_->pull("origin", ""); });
    toolbar->addAction(tr("Push"),   this, [this]() { gitService_->push("origin", ""); });
    toolbar->addSeparator();
    toolbar->addAction(tr("Commit"), this, [this]() {
        if (commitEditorWidget_) {
            QString msg = commitEditorWidget_->message();
            if (!msg.isEmpty()) {
                gitService_->commitChanges(msg);
                commitEditorWidget_->clear();
            }
        }
    });
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
// Dock widgets
// ---------------------------------------------------------------------------
void MainWindow::createDockWidgets()
{
    // Left dock: Branch tree
    auto* branchDock = new QDockWidget(tr("Branches"), this);
    branchTreeWidget_ = new widgets::BranchTreeWidget(branchDock);
    branchDock->setWidget(branchTreeWidget_);
    addDockWidget(Qt::LeftDockWidgetArea, branchDock);

    // Right dock: Staging + Commit editor stacked vertically
    auto* stagingDock = new QDockWidget(tr("Staging"), this);
    stagingWidget_ = new widgets::StagingWidget(stagingDock);
    stagingDock->setWidget(stagingWidget_);
    addDockWidget(Qt::RightDockWidgetArea, stagingDock);

    auto* commitDock = new QDockWidget(tr("Commit"), this);
    commitEditorWidget_ = new widgets::CommitEditorWidget(commitDock);
    commitDock->setWidget(commitEditorWidget_);
    addDockWidget(Qt::RightDockWidgetArea, commitDock);

    // Stack the staging and commit docks in the right area
    tabifyDockWidget(stagingDock, commitDock);
    stagingDock->raise();

    // Bottom dock: Console output
    auto* consoleDock = new QDockWidget(tr("Console"), this);
    consoleWidget_ = new widgets::ConsoleOutputWidget(consoleDock);
    consoleDock->setWidget(consoleWidget_);
    addDockWidget(Qt::BottomDockWidgetArea, consoleDock);
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

    connect(gitService_, &services::GitService::statusReady,
            this, &MainWindow::onStatusReady);

    connect(gitService_, &services::GitService::branchesReady,
            this, &MainWindow::onBranchesReady);

    connect(gitService_, &services::GitService::operationFailed,
            this, [this](const QString& op, const QString& err) {
                statusBar()->showMessage(op + tr(" failed: ") + err, 5000);
                if (consoleWidget_)
                    consoleWidget_->appendError(op + ": " + err);
            });

    connect(gitService_, &services::GitService::commitComplete,
            this, [this](bool success, const QString& message) {
                if (success) {
                    statusBar()->showMessage(message, 3000);
                    if (consoleWidget_)
                        consoleWidget_->appendOutput(message);
                } else {
                    statusBar()->showMessage(tr("Commit failed: ") + message, 5000);
                    if (consoleWidget_)
                        consoleWidget_->appendError(tr("Commit failed: ") + message);
                }
            });

    connect(gitService_, &services::GitService::repositoryChanged,
            this, [this]() {
                gitService_->refreshStatus();
                gitService_->refreshLog();
            });

    // --- CommitLogModel: request more commits (lazy loading) ---
    connect(commitLogModel_, &models::CommitLogModel::requestMoreCommits,
            this, [this](int offset, int count) {
                gitService_->refreshLog(offset, count);
            });

    // --- Dashboard: open-from-recent or clone/init ---
    connect(dashboardView_, &DashboardView::openRepositoryRequested,
            this, [this](const QString& path) {
                if (!gitService_->openRepository(path))
                    QMessageBox::warning(this, tr("Error"),
                        tr("Failed to open repository at %1").arg(path));
            });

    // --- Staging widget signals ---
    if (stagingWidget_) {
        connect(stagingWidget_, &widgets::StagingWidget::stageRequested,
                gitService_, &services::GitService::stageFile);
        connect(stagingWidget_, &widgets::StagingWidget::unstageRequested,
                gitService_, &services::GitService::unstageFile);
        connect(stagingWidget_, &widgets::StagingWidget::stageAllRequested,
                gitService_, &services::GitService::stageAll);
    }

    // --- Commit editor: commit requested ---
    if (commitEditorWidget_) {
        connect(commitEditorWidget_, &widgets::CommitEditorWidget::commitRequested,
                this, [this](const QString& message) {
                    gitService_->commitChanges(message);
                    commitEditorWidget_->clear();
                });
    }

    // --- Branch tree: checkout ---
    if (branchTreeWidget_) {
        connect(branchTreeWidget_, &widgets::BranchTreeWidget::checkoutRequested,
                gitService_, &services::GitService::checkoutBranch);
    }
}

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------
void MainWindow::openRepository()
{
    QString dir = QFileDialog::getExistingDirectory(this, tr("Open Repository"));
    if (!dir.isEmpty()) {
        if (!gitService_->openRepository(dir))
            QMessageBox::warning(this, tr("Error"),
                tr("Failed to open repository at %1").arg(dir));
    }
}

void MainWindow::onRepositoryOpened(const QString& path)
{
    setWindowTitle(QStringLiteral("GitBolt - ") + path);
    centralStack_->setCurrentWidget(repoView_);
    settingsService_->addRecentRepository(path);
    updateRecentMenu();

    // Update status bar with branch name and repo path
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

void MainWindow::onStatusReady(std::vector<gitbolt::git::StatusEntry> /*entries*/)
{
    // Forward status entries to the staging widget.
    // The staging widget's full update implementation is deferred to a later phase;
    // for now this slot exists to complete the wiring.
}

void MainWindow::onBranchesReady(std::vector<gitbolt::git::BranchInfo> /*branches*/)
{
    // Forward branch info to the branch tree widget.
    // Full branch tree model update is deferred to a later phase;
    // for now this slot exists to complete the wiring.

    // Also update the branch label in the status bar
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
    QMessageBox::about(this, tr("About GitBolt"),
        tr("GitBolt - Fast Cross-Platform Git GUI\n\n"
           "Built with Qt 6 and libgit2"));
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
