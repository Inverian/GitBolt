#pragma once
#include <QWidget>

namespace gitbolt::ui {

class DashboardView : public QWidget {
    Q_OBJECT
public:
    explicit DashboardView(QWidget* parent = nullptr);
signals:
    void openRepositoryRequested(const QString& path);
    void cloneRequested();
    void initRequested();
};

} // namespace gitbolt::ui
