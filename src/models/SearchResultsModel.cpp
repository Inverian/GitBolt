#include "models/SearchResultsModel.h"

#include <QDateTime>
#include <QFont>
#include <chrono>

namespace gitbolt::models {

SearchResultsModel::SearchResultsModel(QObject* parent)
    : QAbstractTableModel(parent) {}

int SearchResultsModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid())
        return 0;
    return static_cast<int>(results_.size());
}

int SearchResultsModel::columnCount(const QModelIndex& parent) const {
    if (parent.isValid())
        return 0;
    return ColumnCount;
}

QVariant SearchResultsModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid())
        return {};

    int row = index.row();
    if (row < 0 || row >= static_cast<int>(results_.size()))
        return {};

    const auto& commit = results_[row];

    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case ColHash:
            return QString::fromStdString(commit.id.toShortHex());
        case ColMessage:
            return QString::fromStdString(commit.summary);
        case ColAuthor:
            return QString::fromStdString(commit.author.name);
        case ColDate: {
            auto tp = commit.author.when;
            auto epoch = std::chrono::duration_cast<std::chrono::seconds>(
                             tp.time_since_epoch())
                             .count();
            QDateTime dt = QDateTime::fromSecsSinceEpoch(
                static_cast<qint64>(epoch), Qt::LocalTime);
            return dt.toString(Qt::ISODate);
        }
        default:
            return {};
        }
    }

    if (role == Qt::ToolTipRole) {
        return QString::fromStdString(commit.message);
    }

    if (role == CommitIdRole) {
        return QString::fromStdString(commit.id.toHex());
    }

    if (role == FullMessageRole) {
        return QString::fromStdString(commit.message);
    }

    if (role == Qt::FontRole && index.column() == ColHash) {
        QFont mono(QStringLiteral("Monospace"));
        mono.setStyleHint(QFont::Monospace);
        return mono;
    }

    return {};
}

QVariant SearchResultsModel::headerData(int section, Qt::Orientation orientation,
                                         int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};

    switch (section) {
    case ColHash:    return tr("Hash");
    case ColMessage: return tr("Message");
    case ColAuthor:  return tr("Author");
    case ColDate:    return tr("Date");
    default:         return {};
    }
}

void SearchResultsModel::setResults(std::vector<git::CommitData> results) {
    beginResetModel();
    results_ = std::move(results);
    endResetModel();
}

void SearchResultsModel::clear() {
    beginResetModel();
    results_.clear();
    endResetModel();
}

git::ObjectId SearchResultsModel::commitIdAtRow(int row) const {
    if (row >= 0 && row < static_cast<int>(results_.size()))
        return results_[row].id;
    return {};
}

} // namespace gitbolt::models
