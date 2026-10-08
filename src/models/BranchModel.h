#pragma once

#include "git/Branch.h"
#include "git/Stash.h"
#include "git/Submodule.h"
#include "git/Tag.h"

#include <QAbstractItemModel>
#include <QFont>
#include <QIcon>
#include <vector>

namespace gitbolt::models {

class BranchModel : public QAbstractItemModel {
    Q_OBJECT
public:
    enum class RootCategory : int {
        LocalBranches = 0,
        RemoteBranches = 1,
        Tags = 2,
        Submodules = 3,
        Stashes = 4,
        Count = 5,
    };

    enum Roles {
        FullRefNameRole = Qt::UserRole + 1,
        BranchTypeRole,
        IsHeadRole,
        ObjectIdRole,
    };

    explicit BranchModel(QObject* parent = nullptr);

    // QAbstractItemModel interface
    QModelIndex index(int row, int column,
                      const QModelIndex& parent = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex& child) const override;
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;

    // Data setters
    void setBranches(std::vector<gitbolt::git::BranchInfo> branches);
    void setTags(std::vector<gitbolt::git::TagInfo> tags);
    void setSubmodules(std::vector<gitbolt::git::SubmoduleInfo> submodules);
    void setStashes(std::vector<gitbolt::git::StashEntry> stashes);
    void clear();

    // Queries
    /// Short display name of a branch or tag row ("main", "v1.0").
    QString branchNameAt(const QModelIndex& index) const;
    /// What a checkout of this row should hand Repository::checkout:
    /// the short name for branches, "refs/tags/<name>" for tags.
    QString checkoutRefAt(const QModelIndex& index) const;
    bool isCategoryIndex(const QModelIndex& index) const;

private:
    bool isRootIndex(const QModelIndex& index) const;
    int categoryChildCount(RootCategory cat) const;

    std::vector<gitbolt::git::BranchInfo> localBranches_;
    std::vector<gitbolt::git::BranchInfo> remoteBranches_;
    std::vector<gitbolt::git::TagInfo> tags_;
    std::vector<gitbolt::git::SubmoduleInfo> submodules_;
    std::vector<gitbolt::git::StashEntry> stashes_;
};

} // namespace gitbolt::models
