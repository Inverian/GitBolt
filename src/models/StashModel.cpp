#include "models/StashModel.h"

#include <QDateTime>
#include <chrono>

namespace gitbolt::models {

StashModel::StashModel(QObject* parent)
    : QAbstractTableModel(parent) {}

int StashModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid())
        return 0;
    return static_cast<int>(stashes_.size());
}

int StashModel::columnCount(const QModelIndex& parent) const {
    if (parent.isValid())
        return 0;
    return ColumnCount;
}

QVariant StashModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid())
        return {};

    int row = index.row();
    if (row < 0 || row >= static_cast<int>(stashes_.size()))
        return {};

    const auto& stash = stashes_[row];

    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case ColIndex:
            return QStringLiteral("stash@{%1}").arg(stash.index);
        case ColMessage:
            return QString::fromStdString(stash.message);
        case ColDate: {
            auto tp = stash.author.when;
            auto epoch = std::chrono::duration_cast<std::chrono::seconds>(
                             tp.time_since_epoch())
                             .count();
            QDateTime dt = QDateTime::fromSecsSinceEpoch(
                static_cast<qint64>(epoch), Qt::LocalTime);
            return dt.toString(QStringLiteral("yyyy-MM-dd hh:mm"));
        }
        default:
            return {};
        }
    }

    if (role == Qt::ToolTipRole) {
        return QStringLiteral("stash@{%1}: %2\nBy %3")
            .arg(stash.index)
            .arg(QString::fromStdString(stash.message))
            .arg(QString::fromStdString(stash.author.name));
    }

    if (role == StashIndexRole)
        return static_cast<quint64>(stash.index);

    if (role == ObjectIdRole)
        return QString::fromStdString(stash.id.toHex());

    return {};
}

QVariant StashModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};

    switch (section) {
    case ColIndex:   return tr("Index");
    case ColMessage: return tr("Message");
    case ColDate:    return tr("Date");
    default:         return {};
    }
}

void StashModel::setStashes(std::vector<git::StashEntry> stashes) {
    beginResetModel();
    stashes_ = std::move(stashes);
    endResetModel();
}

void StashModel::clear() {
    beginResetModel();
    stashes_.clear();
    endResetModel();
}

size_t StashModel::stashIndexAtRow(int row) const {
    if (row >= 0 && row < static_cast<int>(stashes_.size()))
        return stashes_[row].index;
    return 0;
}

} // namespace gitbolt::models
