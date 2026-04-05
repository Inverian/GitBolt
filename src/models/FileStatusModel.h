#pragma once
#include "git/Status.h"
#include <QAbstractTableModel>
#include <vector>

namespace gitbolt::models {

class FileStatusModel : public QAbstractTableModel {
    Q_OBJECT
public:
    explicit FileStatusModel(QObject* parent = nullptr);
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    void clear();
    void setEntries(std::vector<gitbolt::git::StatusEntry> entries);
};

} // namespace gitbolt::models
