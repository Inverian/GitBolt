#include "ui/MainWindow.h"
#include "ui/RepositoryView.h"
#include "ui/DashboardView.h"
#include "services/GitService.h"
#include "conf/SettingsService.h"
#include "widgets/BranchTreeWidget.h"
#include "widgets/StagingWidget.h"
#include "widgets/ConsoleOutputWidget.h"
#include <QMenuBar>
#include <QToolBar>
#include <QStatusBar>
#include <QDockWidget>
#include <QStackedWidget>
#include <QFileDialog>
#include <QMessageBox>
#include <QApplication>

namespace gitbolt::ui {

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("GitBolt");
    resize(1280, 800);

    gitService_ = new services::GitService(this);
    settingsService_ = new conf::SettingsService(this);

    auto* stack = new QStackedWidget(this);
    centralStack_ = stack;
    dashboardView_ = new DashboardView(this);
    repoView_ = new RepositoryView(this);
    stack->addWidget(dashboardView_);
    stack->addWidget(repoView_);
    setCentralWidget(stack);

    createMenuBar();
    createToolBar();
    createStatusBar();
    createDockWidgets();
    setupConnections();
}

MainWindow::~MainWindow() = default;

void MainWindow::createMenuBar() {
    auto* file = menuBar()->addMenu(tr("&File"));
    file->addAction(tr("&Open Repository..."), this, &MainWindow::openRepository, QKeySequence::Open);
    file->addSeparator();
    file->addAction(tr("&Quit"), qApp, &QApplication::quit, QKeySequence::Quit);

    auto* repo = menuBar()->addMenu(tr("&Repository"));
    repo->addAction(tr("&Refresh"), [this]() { gitService_->refreshStatus(); gitService_->refreshLog(); }, QKeySequence::Refresh);
    repo->addSeparator();
    repo->addAction(tr("&Fetch"), [this]() { gitService_->fetch(); });
    repo->addAction(tr("Pu&ll"), [this]() { gitService_->pull("origin", ""); });
    repo->addAction(tr("&Push"), [this]() { gitService_->push("origin", ""); });

    auto* help = menuBar()->addMenu(tr("&Help"));
    help->addAction(tr("&About GitBolt"), this, &MainWindow::showAbout);
}

void MainWindow::createToolBar() {
    auto* toolbar = addToolBar(tr("Main"));
    toolbar->setMovable(false);
    toolbar->addAction(tr("Open"), this, &MainWindow::openRepository);
    toolbar->addSeparator();
    toolbar->addAction(tr("Fetch"), [this]() { gitService_->fetch(); });
    toolbar->addAction(tr("Pull"), [this]() { gitService_->pull("origin", ""); });
    toolbar->addAction(tr("Push"), [this]() { gitService_->push("origin", ""); });
}

void MainWindow::createStatusBar() {
    statusBar()->showMessage(tr("Ready"));
}

void MainWindow::createDockWidgets() {
    auto* branchDock = new QDockWidget(tr("Branches"), this);
    branchDock->setWidget(new widgets::BranchTreeWidget(branchDock));
    addDockWidget(Qt::LeftDockWidgetArea, branchDock);

    auto* stagingDock = new QDockWidget(tr("Staging"), this);
    stagingDock->setWidget(new widgets::StagingWidget(stagingDock));
    addDockWidget(Qt::RightDockWidgetArea, stagingDock);

    auto* consoleDock = new QDockWidget(tr("Console"), this);
    consoleDock->setWidget(new widgets::ConsoleOutputWidget(consoleDock));
    addDockWidget(Qt::BottomDockWidgetArea, consoleDock);
}

void MainWindow::setupConnections() {
    connect(gitService_, &services::GitService::repositoryOpened, this, &MainWindow::onRepositoryOpened);
    connect(gitService_, &services::GitService::operationFailed, this, [this](const QString& op, const QString& err) {
        statusBar()->showMessage(op + " failed: " + err, 5000);
    });
}

void MainWindow::openRepository() {
    QString dir = QFileDialog::getExistingDirectory(this, tr("Open Repository"));
    if (!dir.isEmpty()) {
        if (!gitService_->openRepository(dir))
            QMessageBox::warning(this, tr("Error"), tr("Failed to open repository at %1").arg(dir));
    }
}

void MainWindow::onRepositoryOpened(const QString& path) {
    setWindowTitle("GitBolt - " + path);
    static_cast<QStackedWidget*>(centralStack_)->setCurrentWidget(repoView_);
    settingsService_->addRecentRepository(path);
    statusBar()->showMessage(tr("Opened: %1").arg(path));
}

void MainWindow::showAbout() {
    QMessageBox::about(this, tr("About GitBolt"),
        tr("GitBolt - Fast Cross-Platform Git GUI\n\nBuilt with Qt 6 and libgit2"));
}

} // namespace gitbolt::ui
