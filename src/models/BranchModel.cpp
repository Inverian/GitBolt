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
        const auto& b = localBranches_[row];

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
        const auto& b = remoteBranches_[row];

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
        const auto& t = tags_[row];

        switch (role) {
        case Qt::DisplayRole:
            return QString::fromStdString(t.name);
        case Qt::DecorationRole:
            return QIcon::fromTheme(QStringLiteral("tag"));
        case Qt::ToolTipRole: {
            QString tip = QString::fromStdString(t.name);
            tip += QStringLiteral("\nTarget: ") + QString::fromStdString(t.targetId.toShortHex());
            if (t.type == git::TagType::Annotated && !t.message.empty())
                tip += QStringLiteral("\n") + QString::fromStdString(t.message);
            return tip;
        }
        case ObjectIdRole:
            return QString::fromStdString(t.targetId.toHex());
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

void BranchModel::clear() {
    beginResetModel();
    localBranches_.clear();
    remoteBranches_.clear();
    tags_.clear();
    endResetModel();
}

QString BranchModel::branchNameAt(const QModelIndex& index) const {
    if (!index.isValid() || index.internalId() == kRootInternalId)
        return {};

    auto cat = static_cast<RootCategory>(index.internalId());
    int row = index.row();

    if (cat == RootCategory::LocalBranches && row >= 0
        && row < static_cast<int>(localBranches_.size()))
        return QString::fromStdString(localBranches_[row].name);

    if (cat == RootCategory::RemoteBranches && row >= 0
        && row < static_cast<int>(remoteBranches_.size()))
        return QString::fromStdString(remoteBranches_[row].name);

    if (cat == RootCategory::Tags && row >= 0 && row < static_cast<int>(tags_.size()))
        return QString::fromStdString(tags_[row].name);

    return {};
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
    default:
        return 0;
    }
}

} // namespace gitbolt::models
