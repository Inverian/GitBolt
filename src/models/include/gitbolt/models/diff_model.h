#pragma once
#include "gitbolt/core/diff.h"
#include <QAbstractTableModel>
#include <vector>

namespace gitbolt::models {

class DiffModel : public QAbstractTableModel {
    Q_OBJECT
public:
    explicit DiffModel(QObject* parent = nullptr);
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    void clear();
    void setDiffResult(gitbolt::core::DiffResult result);
};

} // namespace gitbolt::models
