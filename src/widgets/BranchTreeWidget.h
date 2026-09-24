#pragma once

#include "models/BranchModel.h"
#include "git/Stash.h"
#include "git/Submodule.h"

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
    void setSubmodules(std::vector<gitbolt::git::SubmoduleInfo> submodules);
    void setStashes(std::vector<gitbolt::git::StashEntry> stashes);
    void clear();

    /// Show or hide a top-level category (Local/Remote branches,
    /// Tags, Submodules, Stashes) in the tree view. Used by the
    /// View menu's "Show remote branches / Show tags / Show
    /// stashes" toggles. Hiding via QTreeView::setRowHidden so
    /// the underlying model is left untouched — toggling back on
    /// is instant.
    void setCategoryVisible(int rootCategory, bool visible);

    models::BranchModel* branchModel() const { return model_; }

signals:
    void branchSelected(const QString& name);
    void checkoutRequested(const QString& name);
    void createBranchRequested(const QString& name);
    void deleteBranchRequested(const QString& name);
    void renameBranchRequested(const QString& oldName, const QString& newName);
    void mergeRequested(const QString& name);
    void pushRequested(const QString& remote, const QString& branch);
    /// A remote-tracking branch ("origin/feature") to delete on its
    /// remote; the receiver confirms before doing anything.
    void deleteRemoteBranchRequested(const QString& remoteBranch);
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
