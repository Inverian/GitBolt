#pragma once
#include "gitbolt/core/tag.h"
#include <QAbstractTableModel>
#include <vector>

namespace gitbolt::models {

class TagModel : public QAbstractTableModel {
    Q_OBJECT
public:
    explicit TagModel(QObject* parent = nullptr);
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    void clear();
    void setTags(std::vector<gitbolt::core::TagInfo> tags);
};

} // namespace gitbolt::models
