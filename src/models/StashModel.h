#pragma once
#include "git/Stash.h"
#include <QAbstractTableModel>
#include <vector>

namespace gitbolt::models {

class StashModel : public QAbstractTableModel {
    Q_OBJECT
public:
    explicit StashModel(QObject* parent = nullptr);
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    void clear();
    void setStashes(std::vector<gitbolt::git::StashEntry> stashes);
};

} // namespace gitbolt::models
