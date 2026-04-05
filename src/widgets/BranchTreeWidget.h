#pragma once

#include "models/BranchModel.h"

#include <QLineEdit>
#include <QMenu>
#include <QSortFilterProxyModel>
#include <QToolBar>
#include <QTreeView>
#include <QWidget>

namespace gitbolt::widgets {

class BranchTreeWidget : public QWidget {
    Q_OBJECT
public:
    explicit BranchTreeWidget(QWidget* parent = nullptr);

    void setBranches(std::vector<gitbolt::git::BranchInfo> branches);
    void setTags(std::vector<gitbolt::git::TagInfo> tags);
    void clear();

    models::BranchModel* branchModel() const { return model_; }

signals:
    void branchSelected(const QString& name);
    void checkoutRequested(const QString& name);
    void createBranchRequested(const QString& name);
    void deleteBranchRequested(const QString& name);
    void renameBranchRequested(const QString& oldName, const QString& newName);
    void mergeRequested(const QString& name);
    void pushRequested(const QString& remote, const QString& branch);
    void setUpstreamRequested(const QString& branch, const QString& upstream);

private slots:
    void onDoubleClicked(const QModelIndex& index);
    void onCustomContextMenu(const QPoint& pos);
    void onFilterChanged(const QString& text);
    void onNewBranchClicked();

private:
    void setupToolbar();
    void setupContextMenu(const QModelIndex& index, const QPoint& globalPos);
    void expandLocalBranches();

    QTreeView* treeView_ = nullptr;
    models::BranchModel* model_ = nullptr;
    QSortFilterProxyModel* filterProxy_ = nullptr;
    QToolBar* toolbar_ = nullptr;
    QLineEdit* filterInput_ = nullptr;
};

} // namespace gitbolt::widgets
