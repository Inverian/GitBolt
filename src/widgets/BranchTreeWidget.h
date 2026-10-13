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

#include <functional>
#include <string>

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

    /// Off while the window keeps the repository's actions off (no
    /// repository, an open on its way, a local op or a step running):
    /// New Branch and every context-menu entry are disabled, and a
    /// double-click checks nothing out. They asked for a name or a
    /// confirmation first, then had it thrown away with a passing
    /// "Another operation is still running…".
    void setRepoActionsEnabled(bool on);
    bool repoActionsEnabled() const { return repoActionsEnabled_; }

    /// Where the key of the repository the sidebar shows comes from
    /// (GitService::openKey()): every request below carries the one it
    /// was made in, a context menu's the one open when the menu went up.
    /// Taken once the entry was picked, after the menu's own event loop,
    /// it was whichever repository a second launch had handed over
    /// meanwhile: B's "feature" was renamed, deleted or pushed from A's
    /// menu.
    void setRepositoryKeySource(std::function<std::string()> source);
    /// The key the source gives now; empty without one.
    std::string repositoryKey() const;

signals:
    // Every request carries `repoKey`, the repositoryKey() of the
    // repository it was made in: the receiver does nothing once another
    // one is open.
    void branchSelected(const QString& name);
    void checkoutRequested(const QString& name, const std::string& repoKey);
    /// "Checkout as local branch" on a remote branch ("origin/x").
    void checkoutRemoteRequested(const QString& remoteBranch, const std::string& repoKey);
    /// New Branch: the toolbar's, from HEAD (`startPoint` empty), or a
    /// local branch's "New Branch from '<it>'", from that branch. The
    /// receiver asks for the name, as it does for Rename's and Set
    /// Upstream's and for Delete's confirmation, so that what it does
    /// goes to the repository asked about.
    void createBranchRequested(const QString& startPoint, const std::string& repoKey);
    /// The context menu's Delete on a local branch. Not asked here:
    /// the receiver confirms first.
    void deleteBranchRequested(const QString& name, const std::string& repoKey);
    /// Rename on a local branch; the receiver asks for the new name.
    void renameBranchRequested(const QString& name, const std::string& repoKey);
    void mergeRequested(const QString& name, const std::string& repoKey);
    void pushRequested(const QString& remote, const QString& branch, const std::string& repoKey);
    /// A remote-tracking branch ("origin/feature") to delete on its
    /// remote; the receiver confirms before doing anything.
    void deleteRemoteBranchRequested(const QString& remoteBranch, const std::string& repoKey);
    /// Set Upstream on a local branch; the receiver asks for it.
    void setUpstreamRequested(const QString& branch, const std::string& repoKey);

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
    QAction* newBranchAction_ = nullptr;
    bool repoActionsEnabled_ = true;
    std::function<std::string()> keySource_;
};

} // namespace gitbolt::widgets
