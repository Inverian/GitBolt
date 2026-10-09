#include "models/BranchModel.h"

#include <QDateTime>
#include <QString>
#include <algorithm>

namespace gitbolt::models {

// Internal IDs:
// Root categories use internalId = 0xFFFFFFFF (quintptr max).
// Children of a root category use internalId = static_cast<quintptr>(RootCategory).

static constexpr quintptr kRootInternalId = std::numeric_limits<quintptr>::max();

BranchModel::BranchModel(QObject* parent)
    : QAbstractItemModel(parent) {}

QModelIndex BranchModel::index(int row, int column, const QModelIndex& parent) const {
    if (!hasIndex(row, column, parent))
        return {};

    if (!parent.isValid()) {
        // Top-level: root category nodes
        return createIndex(row, column, kRootInternalId);
    }

    if (parent.internalId() == kRootInternalId) {
        // Child of a root category. Store the category enum as internalId.
        auto cat = static_cast<quintptr>(parent.row());
        return createIndex(row, column, cat);
    }

    // No deeper nesting.
    return {};
}

QModelIndex BranchModel::parent(const QModelIndex& child) const {
    if (!child.isValid())
        return {};

    if (child.internalId() == kRootInternalId) {
        // Root category has no parent.
        return {};
    }

    // Child belongs to a root category; reconstruct the parent index.
    auto catRow = static_cast<int>(child.internalId());
    return createIndex(catRow, 0, kRootInternalId);
}

int BranchModel::rowCount(const QModelIndex& parent) const {
    if (!parent.isValid()) {
        // Number of root categories.
        return static_cast<int>(RootCategory::Count);
    }

    if (parent.internalId() == kRootInternalId) {
        auto cat = static_cast<RootCategory>(parent.row());
        return categoryChildCount(cat);
    }

    return 0;
}

int BranchModel::columnCount(const QModelIndex& /*parent*/) const {
    return 1;
}

QVariant BranchModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid())
        return {};

    // Root category nodes
    if (index.internalId() == kRootInternalId) {
        if (role == Qt::DisplayRole) {
            switch (static_cast<RootCategory>(index.row())) {
            case RootCategory::LocalBranches:
                return tr("Local Branches");
            case RootCategory::RemoteBranches:
                return tr("Remote Branches");
            case RootCategory::Tags:
                return tr("Tags");
            case RootCategory::Submodules:
                return tr("Submodules");
            case RootCategory::Stashes:
                return tr("Stashes");
            default:
                return {};
            }
        }
        if (role == Qt::FontRole) {
            QFont f;
            f.setBold(true);
            return f;
        }
        if (role == Qt::DecorationRole) {
            switch (static_cast<RootCategory>(index.row())) {
            case RootCategory::LocalBranches:
                return QIcon::fromTheme(QStringLiteral("vcs-branch"));
            case RootCategory::RemoteBranches:
                return QIcon::fromTheme(QStringLiteral("network-server"));
            case RootCategory::Tags:
                return QIcon::fromTheme(QStringLiteral("tag"));
            case RootCategory::Submodules:
                return QIcon::fromTheme(QStringLiteral("folder"));
            case RootCategory::Stashes:
                return QIcon::fromTheme(QStringLiteral("document-save"));
            default:
                return {};
            }
        }
        return {};
    }

    // Child items
    auto cat = static_cast<RootCategory>(index.internalId());
    int row = index.row();

    if (cat == RootCategory::LocalBranches) {
        if (row < 0 || row >= static_cast<int>(localBranches_.size()))
            return {};
        const auto& b = localBranches_[static_cast<size_t>(row)];

        switch (role) {
        case Qt::DisplayRole: {
            QString display = QString::fromStdString(b.name);
            if (b.aheadCount > 0 || b.behindCount > 0) {
                display += QStringLiteral(" [");
                if (b.aheadCount > 0)
                    display += QStringLiteral("+%1").arg(b.aheadCount);
                if (b.behindCount > 0) {
                    if (b.aheadCount > 0) display += QStringLiteral("/");
                    display += QStringLiteral("-%1").arg(b.behindCount);
                }
                display += QStringLiteral("]");
            }
            return display;
        }
        case Qt::FontRole: {
            if (b.isHead) {
                QFont f;
                f.setBold(true);
                return f;
            }
            return {};
        }
        case Qt::DecorationRole:
            return b.isHead ? QIcon::fromTheme(QStringLiteral("vcs-branch"))
                            : QIcon::fromTheme(QStringLiteral("vcs-branch"));
        case Qt::ToolTipRole: {
            QString tip = QString::fromStdString(b.fullRefName);
            if (!b.upstream.empty())
                tip += QStringLiteral("\nUpstream: ") + QString::fromStdString(b.upstream);
            if (b.isHead)
                tip += QStringLiteral("\n(HEAD)");
            return tip;
        }
        case FullRefNameRole:
            return QString::fromStdString(b.fullRefName);
        case BranchTypeRole:
            return static_cast<int>(b.type);
        case IsHeadRole:
            return b.isHead;
        case ObjectIdRole:
            return QString::fromStdString(b.tipId.toHex());
        default:
            return {};
        }
    }

    if (cat == RootCategory::RemoteBranches) {
        if (row < 0 || row >= static_cast<int>(remoteBranches_.size()))
            return {};
        const auto& b = remoteBranches_[static_cast<size_t>(row)];

        switch (role) {
        case Qt::DisplayRole:
            return QString::fromStdString(b.name);
        case Qt::DecorationRole:
            return QIcon::fromTheme(QStringLiteral("network-server"));
        case Qt::ToolTipRole:
            return QString::fromStdString(b.fullRefName);
        case FullRefNameRole:
            return QString::fromStdString(b.fullRefName);
        case BranchTypeRole:
            return static_cast<int>(b.type);
        case IsHeadRole:
            return false;
        case ObjectIdRole:
            return QString::fromStdString(b.tipId.toHex());
        default:
            return {};
        }
    }

    if (cat == RootCategory::Tags) {
        if (row < 0 || row >= static_cast<int>(tags_.size()))
            return {};
        const auto& t = tags_[static_cast<size_t>(row)];

        switch (role) {
        case Qt::DisplayRole:
            return QString::fromStdString(t.name);
        case Qt::DecorationRole:
            return QIcon::fromTheme(QStringLiteral("tag"));
        case Qt::ToolTipRole: {
            QString tip = QString::fromStdString(t.fullRefName);
            tip += QStringLiteral("\nTarget: ") + QString::fromStdString(t.targetId.toShortHex());
            if (t.type == git::TagType::Annotated) {
                if (!t.tagger.name.empty())
                    tip += QStringLiteral("\n") + tr("Tagger: %1 <%2>").arg(
                        QString::fromStdString(t.tagger.name),
                        QString::fromStdString(t.tagger.email));
                // git ends a tag message with a newline.
                const QString message = QString::fromStdString(t.message).trimmed();
                if (!message.isEmpty())
                    tip += QStringLiteral("\n") + message;
            }
            return tip;
        }
        case FullRefNameRole:
            return QString::fromStdString(t.fullRefName);
        case ObjectIdRole:
            return QString::fromStdString(t.targetId.toHex());
        default:
            return {};
        }
    }

    if (cat == RootCategory::Submodules) {
        if (row < 0 || row >= static_cast<int>(submodules_.size()))
            return {};
        const auto& sm = submodules_[static_cast<size_t>(row)];

        switch (role) {
        case Qt::DisplayRole:
            return QString::fromStdString(sm.name);
        case Qt::DecorationRole:
            return QIcon::fromTheme(QStringLiteral("folder"));
        case Qt::ToolTipRole: {
            QString tip = QString::fromStdString(sm.name);
            if (!sm.url.empty())
                tip += QStringLiteral("\nURL: ") + QString::fromStdString(sm.url);
            if (!sm.branch.empty())
                tip += QStringLiteral("\nBranch: ") + QString::fromStdString(sm.branch);
            return tip;
        }
        default:
            return {};
        }
    }

    if (cat == RootCategory::Stashes) {
        if (row < 0 || row >= static_cast<int>(stashes_.size()))
            return {};
        const auto& s = stashes_[static_cast<size_t>(row)];

        switch (role) {
        case Qt::DisplayRole:
            return QString::fromStdString(s.message);
        case Qt::DecorationRole:
            return QIcon::fromTheme(QStringLiteral("document-save"));
        case Qt::ToolTipRole: {
            QString tip = QStringLiteral("stash@{%1}").arg(s.index);
            tip += QStringLiteral("\n") + QString::fromStdString(s.message);
            return tip;
        }
        default:
            return {};
        }
    }

    return {};
}

QVariant BranchModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation == Qt::Horizontal && role == Qt::DisplayRole && section == 0)
        return tr("Refs");
    return {};
}

Qt::ItemFlags BranchModel::flags(const QModelIndex& index) const {
    if (!index.isValid())
        return Qt::NoItemFlags;

    Qt::ItemFlags flags = Qt::ItemIsEnabled;

    // Root categories are not selectable.
    if (index.internalId() != kRootInternalId)
        flags |= Qt::ItemIsSelectable;

    return flags;
}

void BranchModel::setBranches(std::vector<git::BranchInfo> branches) {
    beginResetModel();
    localBranches_.clear();
    remoteBranches_.clear();

    for (auto& b : branches) {
        if (b.type == git::BranchType::Local)
            localBranches_.push_back(std::move(b));
        else
            remoteBranches_.push_back(std::move(b));
    }

    // Sort: HEAD branch first among locals, then alphabetical.
    std::sort(localBranches_.begin(), localBranches_.end(),
              [](const git::BranchInfo& a, const git::BranchInfo& b) {
                  if (a.isHead != b.isHead) return a.isHead;
                  return a.name < b.name;
              });

    std::sort(remoteBranches_.begin(), remoteBranches_.end(),
              [](const git::BranchInfo& a, const git::BranchInfo& b) {
                  return a.name < b.name;
              });

    endResetModel();
}

void BranchModel::setTags(std::vector<git::TagInfo> tags) {
    beginResetModel();
    tags_ = std::move(tags);
    std::sort(tags_.begin(), tags_.end(),
              [](const git::TagInfo& a, const git::TagInfo& b) {
                  return a.name < b.name;
              });
    endResetModel();
}

void BranchModel::setSubmodules(std::vector<git::SubmoduleInfo> submodules) {
    beginResetModel();
    submodules_ = std::move(submodules);
    std::sort(submodules_.begin(), submodules_.end(),
              [](const git::SubmoduleInfo& a, const git::SubmoduleInfo& b) {
                  return a.name < b.name;
              });
    endResetModel();
}

void BranchModel::setStashes(std::vector<git::StashEntry> stashes) {
    beginResetModel();
    stashes_ = std::move(stashes);
    // Stashes are already ordered by index (most recent first).
    endResetModel();
}

void BranchModel::clear() {
    beginResetModel();
    localBranches_.clear();
    remoteBranches_.clear();
    tags_.clear();
    submodules_.clear();
    stashes_.clear();
    endResetModel();
}

QString BranchModel::branchNameAt(const QModelIndex& index) const {
    if (!index.isValid() || index.internalId() == kRootInternalId)
        return {};

    auto cat = static_cast<RootCategory>(index.internalId());
    int row = index.row();

    if (cat == RootCategory::LocalBranches && row >= 0
        && row < static_cast<int>(localBranches_.size()))
        return QString::fromStdString(localBranches_[static_cast<size_t>(row)].name);

    if (cat == RootCategory::RemoteBranches && row >= 0
        && row < static_cast<int>(remoteBranches_.size()))
        return QString::fromStdString(remoteBranches_[static_cast<size_t>(row)].name);

    if (cat == RootCategory::Tags && row >= 0 && row < static_cast<int>(tags_.size()))
        return QString::fromStdString(tags_[static_cast<size_t>(row)].name);

    return {};
}

QString BranchModel::checkoutRefAt(const QModelIndex& index) const {
    // Repository::checkout follows git: a short name that is also a
    // LOCAL BRANCH checks out the branch, so a tag row handing over
    // its short name would land on a same-named branch instead. The
    // full ref can't collide. Branches stay short: checkout only
    // attaches HEAD for a bare local branch name, so "refs/heads/x"
    // would detach.
    if (index.isValid() && index.internalId() == static_cast<quintptr>(RootCategory::Tags)) {
        const int row = index.row();
        if (row >= 0 && row < static_cast<int>(tags_.size()))
            return QString::fromStdString(tags_[static_cast<size_t>(row)].fullRefName);
        return {};
    }
    return branchNameAt(index);
}

bool BranchModel::isCategoryIndex(const QModelIndex& index) const {
    return index.isValid() && index.internalId() == kRootInternalId;
}

int BranchModel::categoryChildCount(RootCategory cat) const {
    switch (cat) {
    case RootCategory::LocalBranches:
        return static_cast<int>(localBranches_.size());
    case RootCategory::RemoteBranches:
        return static_cast<int>(remoteBranches_.size());
    case RootCategory::Tags:
        return static_cast<int>(tags_.size());
    case RootCategory::Submodules:
        return static_cast<int>(submodules_.size());
    case RootCategory::Stashes:
        return static_cast<int>(stashes_.size());
    default:
        return 0;
    }
}

} // namespace gitbolt::models
