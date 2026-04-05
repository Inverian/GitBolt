#pragma once

#include "git/Branch.h"
#include "git/Commit.h"
#include "git/Status.h"

#include <QMainWindow>
#include <vector>

class QStackedWidget;
class QLabel;
class QMenu;

namespace gitbolt::models { class CommitLogModel; }
namespace gitbolt::services { class GitService; }
namespace gitbolt::conf { class SettingsService; }

namespace gitbolt::widgets {
class BranchTreeWidget;
class StagingWidget;
class CommitEditorWidget;
class ConsoleOutputWidget;
} // namespace gitbolt::widgets

namespace gitbolt::ui {

class RepositoryView;
class DashboardView;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private slots:
    void openRepository();
    void onRepositoryOpened(const QString& path);
    void onLogReady(std::vector<gitbolt::git::CommitData> commits, int offset);
    void onStatusReady(std::vector<gitbolt::git::StatusEntry> entries);
    void onBranchesReady(std::vector<gitbolt::git::BranchInfo> branches);
    void showAbout();

private:
    void createMenuBar();
    void createToolBar();
    void createStatusBar();
    void createDockWidgets();
    void setupConnections();
    void updateRecentMenu();

    services::GitService* gitService_ = nullptr;
    conf::SettingsService* settingsService_ = nullptr;
    models::CommitLogModel* commitLogModel_ = nullptr;

    QStackedWidget* centralStack_ = nullptr;
    DashboardView* dashboardView_ = nullptr;
    RepositoryView* repoView_ = nullptr;

    // Dock widget contents
    widgets::BranchTreeWidget* branchTreeWidget_ = nullptr;
    widgets::StagingWidget* stagingWidget_ = nullptr;
    widgets::CommitEditorWidget* commitEditorWidget_ = nullptr;
    widgets::ConsoleOutputWidget* consoleWidget_ = nullptr;

    // Status bar labels
    QLabel* branchLabel_ = nullptr;
    QLabel* repoPathLabel_ = nullptr;

    // Menus
    QMenu* recentMenu_ = nullptr;
};

} // namespace gitbolt::ui
