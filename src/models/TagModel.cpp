#include "models/TagModel.h"

#include <QFont>
#include <QIcon>

namespace gitbolt::models {

TagModel::TagModel(QObject* parent)
    : QAbstractTableModel(parent) {}

int TagModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid())
        return 0;
    return static_cast<int>(tags_.size());
}

int TagModel::columnCount(const QModelIndex& parent) const {
    if (parent.isValid())
        return 0;
    return ColumnCount;
}

QVariant TagModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid())
        return {};

    int row = index.row();
    if (row < 0 || row >= static_cast<int>(tags_.size()))
        return {};

    const auto& tag = tags_[row];

    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case ColName:
            return QString::fromStdString(tag.name);
        case ColTarget:
            return QString::fromStdString(tag.targetId.toShortHex());
        case ColType:
            return (tag.type == git::TagType::Annotated)
                       ? tr("Annotated")
                       : tr("Lightweight");
        default:
            return {};
        }
    }

    if (role == Qt::ToolTipRole) {
        QString tip = QString::fromStdString(tag.name);
        tip += QStringLiteral("\nTarget: ") +
               QString::fromStdString(tag.targetId.toShortHex());
        if (tag.type == git::TagType::Annotated) {
            if (!tag.message.empty())
                tip += QStringLiteral("\n") + QString::fromStdString(tag.message);
            if (!tag.tagger.name.empty())
                tip += QStringLiteral("\nBy: ") +
                       QString::fromStdString(tag.tagger.name);
        }
        return tip;
    }

    if (role == Qt::DecorationRole && index.column() == ColName) {
        return QIcon::fromTheme(QStringLiteral("tag"));
    }

    if (role == Qt::FontRole && index.column() == ColTarget) {
        QFont mono(QStringLiteral("Monospace"));
        mono.setStyleHint(QFont::Monospace);
        return mono;
    }

    if (role == TagNameRole)
        return QString::fromStdString(tag.name);

    if (role == TargetIdRole)
        return QString::fromStdString(tag.targetId.toHex());

    if (role == TagIdRole)
        return QString::fromStdString(tag.tagId.toHex());

    if (role == TagTypeRole)
        return static_cast<int>(tag.type);

    return {};
}

QVariant TagModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};

    switch (section) {
    case ColName:   return tr("Name");
    case ColTarget: return tr("Target");
    case ColType:   return tr("Type");
    default:        return {};
    }
}

void TagModel::setTags(std::vector<git::TagInfo> tags) {
    beginResetModel();
    tags_ = std::move(tags);
    endResetModel();
}

void TagModel::clear() {
    beginResetModel();
    tags_.clear();
    endResetModel();
}

QString TagModel::tagNameAtRow(int row) const {
    if (row >= 0 && row < static_cast<int>(tags_.size()))
        return QString::fromStdString(tags_[row].name);
    return {};
}

} // namespace gitbolt::models
