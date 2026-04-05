#pragma once

#include <QWidget>

class QLabel;
class QListWidget;
class QPushButton;
class QGroupBox;

namespace gitbolt::services {
class GitService;
}

namespace gitbolt::widgets {

class ConsoleOutputWidget;

class GitFlowWidget : public QWidget {
    Q_OBJECT
public:
    explicit GitFlowWidget(QWidget* parent = nullptr);

    void setGitService(services::GitService* service);
    void refresh();

signals:
    void featureStarted(const QString& name);
    void featureFinished(const QString& name);
    void releaseStarted(const QString& version);
    void releaseFinished(const QString& version);
    void hotfixStarted(const QString& version);
    void hotfixFinished(const QString& version);

private slots:
    void onInitializeGitFlow();
    void onStartFeature();
    void onFinishFeature();
    void onStartRelease();
    void onFinishRelease();
    void onStartHotfix();
    void onFinishHotfix();
    void onGitFlowOperationComplete(bool success, const QString& message);

private:
    void setupUi();
    void updateUiState();

    services::GitService* gitService_ = nullptr;

    // Status
    QLabel* statusLabel_ = nullptr;
    QPushButton* initBtn_ = nullptr;

    // Feature section
    QGroupBox* featureGroup_ = nullptr;
    QListWidget* featureList_ = nullptr;
    QPushButton* startFeatureBtn_ = nullptr;
    QPushButton* finishFeatureBtn_ = nullptr;

    // Release section
    QGroupBox* releaseGroup_ = nullptr;
    QListWidget* releaseList_ = nullptr;
    QPushButton* startReleaseBtn_ = nullptr;
    QPushButton* finishReleaseBtn_ = nullptr;

    // Hotfix section
    QGroupBox* hotfixGroup_ = nullptr;
    QListWidget* hotfixList_ = nullptr;
    QPushButton* startHotfixBtn_ = nullptr;
    QPushButton* finishHotfixBtn_ = nullptr;

    // Console output
    ConsoleOutputWidget* console_ = nullptr;
};

} // namespace gitbolt::widgets
