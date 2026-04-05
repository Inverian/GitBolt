#pragma once
#include "gitbolt/core/blame.h"
#include <QAbstractTableModel>
#include <vector>

namespace gitbolt::models {

class BlameModel : public QAbstractTableModel {
    Q_OBJECT
public:
    explicit BlameModel(QObject* parent = nullptr);
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    void clear();
    void setBlameResult(gitbolt::core::BlameResult result);
};

} // namespace gitbolt::models
