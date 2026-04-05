#pragma once
#include <QMainWindow>

namespace gitbolt::services { class GitService; }
namespace gitbolt::conf { class SettingsService; }

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
    void showAbout();

private:
    void createMenuBar();
    void createToolBar();
    void createStatusBar();
    void createDockWidgets();
    void setupConnections();

    services::GitService* gitService_;
    services::SettingsService* settingsService_;
    QWidget* centralStack_;
    DashboardView* dashboardView_;
    RepositoryView* repoView_;
};

} // namespace gitbolt::ui
