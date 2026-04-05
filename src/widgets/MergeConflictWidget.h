#pragma once
#include <QWidget>

namespace gitbolt::widgets {

class MergeConflictWidget : public QWidget {
    Q_OBJECT
public:
    explicit MergeConflictWidget(QWidget* parent = nullptr);
signals:
    void conflictResolved();

};

} // namespace gitbolt::widgets
