#pragma once

#include "git/Tag.h"

#include <QAbstractTableModel>
#include <vector>

namespace gitbolt::models {

class TagModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column {
        ColName = 0,
        ColTarget,
        ColType,
        ColumnCount,
    };

    enum Roles {
        TagNameRole = Qt::UserRole + 1,
        TargetIdRole,
        TagIdRole,
        TagTypeRole,
    };

    explicit TagModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

    void setTags(std::vector<gitbolt::git::TagInfo> tags);
    void clear();

    QString tagNameAtRow(int row) const;

private:
    std::vector<gitbolt::git::TagInfo> tags_;
};

} // namespace gitbolt::models
