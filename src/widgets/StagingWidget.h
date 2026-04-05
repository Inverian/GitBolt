#pragma once
#include <QWidget>

namespace gitbolt::widgets {

class StagingWidget : public QWidget {
    Q_OBJECT
public:
    explicit StagingWidget(QWidget* parent = nullptr);
signals:
    void stageRequested(const QString& path);
    void unstageRequested(const QString& path);
    void stageAllRequested();

};

} // namespace gitbolt::widgets
