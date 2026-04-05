#pragma once

#include <QWidget>

class QLabel;
class QPushButton;
class QTextEdit;

namespace gitbolt::services {
class GitService;
}

namespace gitbolt::widgets {

class ConsoleOutputWidget;

class MaintenanceWidget : public QWidget {
    Q_OBJECT
public:
    explicit MaintenanceWidget(QWidget* parent = nullptr);

    void setGitService(services::GitService* service);
    void refreshStats();

private slots:
    void onGarbageCollect();
    void onPrune();
    void onFsck();
    void onRepack();
    void onRunAll();
    void onMaintenanceComplete(const QString& output);

private:
    void setupUi();
    void setButtonsEnabled(bool enabled);
    void captureBeforeDiskUsage();

    services::GitService* gitService_ = nullptr;

    QLabel* statsLabel_ = nullptr;
    QPushButton* gcBtn_ = nullptr;
    QPushButton* pruneBtn_ = nullptr;
    QPushButton* fsckBtn_ = nullptr;
    QPushButton* repackBtn_ = nullptr;
    QPushButton* runAllBtn_ = nullptr;
    QLabel* comparisonLabel_ = nullptr;
    ConsoleOutputWidget* console_ = nullptr;

    QString beforeDiskUsage_;
    int runAllStep_ = 0;
};

} // namespace gitbolt::widgets
