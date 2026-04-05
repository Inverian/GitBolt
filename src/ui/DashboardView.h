#pragma once

#include <QWidget>

class QLabel;
class QListWidget;
class QPushButton;

namespace gitbolt::conf {
class SettingsService;
} // namespace gitbolt::conf

namespace gitbolt::ui {

class DashboardView : public QWidget {
    Q_OBJECT
public:
    explicit DashboardView(conf::SettingsService* settings, QWidget* parent = nullptr);

    /// Refresh the recent-repositories list from SettingsService.
    void refreshRecentList();

signals:
    void openRepositoryRequested(const QString& path);
    void cloneRequested();
    void initRequested(const QString& path);

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    void setupUi();
    QPushButton* createActionCard(const QString& title, const QString& iconText,
                                  const QString& description);

    conf::SettingsService* settings_ = nullptr;

    QLabel* titleLabel_ = nullptr;
    QPushButton* openBtn_ = nullptr;
    QPushButton* cloneBtn_ = nullptr;
    QPushButton* initBtn_ = nullptr;
    QListWidget* recentList_ = nullptr;
    QPushButton* clearRecentBtn_ = nullptr;
    QLabel* noRecentLabel_ = nullptr;
};

} // namespace gitbolt::ui
