#pragma once
#include "gitbolt/core/branch.h"
#include <QAbstractItemModel>
#include <vector>

namespace gitbolt::models {

class BranchModel : public QAbstractItemModel {
    Q_OBJECT
public:
    explicit BranchModel(QObject* parent = nullptr);
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    void clear();
    void setBranches(std::vector<gitbolt::core::BranchInfo> branches);
};

} // namespace gitbolt::models
