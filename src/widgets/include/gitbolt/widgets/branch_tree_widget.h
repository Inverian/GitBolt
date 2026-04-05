#pragma once
#include <QWidget>

namespace gitbolt::widgets {

class BranchTreeWidget : public QWidget {
    Q_OBJECT
public:
    explicit BranchTreeWidget(QWidget* parent = nullptr);
signals:
    void branchSelected(const QString& name);
    void checkoutRequested(const QString& name);

};

} // namespace gitbolt::widgets
