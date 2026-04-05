#include "models/ReflogModel.h"

#include <QDateTime>
#include <QIcon>
#include <chrono>

namespace gitbolt::models {

ReflogModel::ReflogModel(QObject* parent)
    : QAbstractTableModel(parent) {}

int ReflogModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid())
        return 0;
    return static_cast<int>(entries_.size());
}

int ReflogModel::columnCount(const QModelIndex& parent) const {
    if (parent.isValid())
        return 0;
    return ColumnCount;
}

QVariant ReflogModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid())
        return {};

    int row = index.row();
    if (row < 0 || row >= static_cast<int>(entries_.size()))
        return {};

    const auto& entry = entries_[row];

    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case ColAction:
            return QString::fromStdString(entry.action);
        case ColHash:
            return QString::fromStdString(entry.id.toShortHex());
        case ColMessage:
            return QString::fromStdString(entry.message);
        case ColDate: {
            auto epoch = std::chrono::duration_cast<std::chrono::seconds>(
                             entry.timestamp.time_since_epoch())
                             .count();
            QDateTime dt = QDateTime::fromSecsSinceEpoch(
                static_cast<qint64>(epoch), Qt::LocalTime);
            return dt.toString(QStringLiteral("yyyy-MM-dd hh:mm:ss"));
        }
        default:
            return {};
        }
    }

    if (role == Qt::ToolTipRole) {
        return QStringLiteral("%1: %2\n%3")
            .arg(QString::fromStdString(entry.action))
            .arg(QString::fromStdString(entry.message))
            .arg(QString::fromStdString(entry.id.toShortHex()));
    }

    if (role == Qt::DecorationRole && index.column() == ColAction) {
        // Provide visual cues for common actions
        const auto& act = entry.action;
        if (act == "commit" || act == "commit (initial)" || act == "commit (amend)")
            return QIcon::fromTheme(QStringLiteral("vcs-commit"));
        if (act.find("checkout") != std::string::npos)
            return QIcon::fromTheme(QStringLiteral("go-jump"));
        if (act.find("merge") != std::string::npos)
            return QIcon::fromTheme(QStringLiteral("vcs-merge"));
        if (act.find("rebase") != std::string::npos)
            return QIcon::fromTheme(QStringLiteral("view-refresh"));
        if (act.find("pull") != std::string::npos)
            return QIcon::fromTheme(QStringLiteral("go-down"));
        if (act.find("reset") != std::string::npos)
            return QIcon::fromTheme(QStringLiteral("edit-undo"));
        return QIcon::fromTheme(QStringLiteral("dialog-information"));
    }

    if (role == Qt::FontRole && index.column() == ColHash) {
        QFont mono(QStringLiteral("Monospace"));
        mono.setStyleHint(QFont::Monospace);
        return mono;
    }

    if (role == ObjectIdRole)
        return QString::fromStdString(entry.id.toHex());

    if (role == ActionRole)
        return QString::fromStdString(entry.action);

    if (role == FullMessageRole)
        return QString::fromStdString(entry.message);

    return {};
}

QVariant ReflogModel::headerData(int section, Qt::Orientation orientation,
                                  int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};

    switch (section) {
    case ColAction:  return tr("Action");
    case ColHash:    return tr("Hash");
    case ColMessage: return tr("Message");
    case ColDate:    return tr("Date");
    default:         return {};
    }
}

void ReflogModel::setEntries(std::vector<ReflogEntry> entries) {
    beginResetModel();
    entries_ = std::move(entries);
    endResetModel();
}

void ReflogModel::clear() {
    beginResetModel();
    entries_.clear();
    endResetModel();
}

git::ObjectId ReflogModel::objectIdAtRow(int row) const {
    if (row >= 0 && row < static_cast<int>(entries_.size()))
        return entries_[row].id;
    return {};
}

} // namespace gitbolt::models
