#pragma once

#include "git/ObjectId.h"
#include "models/ReflogModel.h"

#include <QAction>
#include <QTableView>
#include <QToolBar>
#include <QWidget>

namespace gitbolt::widgets {

class ReflogWidget : public QWidget {
    Q_OBJECT
public:
    explicit ReflogWidget(QWidget* parent = nullptr);

    void setEntries(std::vector<gitbolt::models::ReflogEntry> entries);
    void clear();

    models::ReflogModel* reflogModel() const { return model_; }

signals:
    void refreshRequested();
    void checkoutRequested(const gitbolt::git::ObjectId& id);
    void resetRequested(const gitbolt::git::ObjectId& id);
    void commitSelected(const gitbolt::git::ObjectId& id);

private slots:
    void onRowDoubleClicked(const QModelIndex& index);
    void onContextMenu(const QPoint& pos);

private:
    void setupUI();

    QTableView* tableView_ = nullptr;
    models::ReflogModel* model_ = nullptr;
    QToolBar* toolbar_ = nullptr;
    QAction* refreshAction_ = nullptr;
};

} // namespace gitbolt::widgets
