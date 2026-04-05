#pragma once
#include "gitbolt/core/commit.h"
#include <QAbstractTableModel>
#include <vector>

namespace gitbolt::models {

class SearchResultsModel : public QAbstractTableModel {
    Q_OBJECT
public:
    explicit SearchResultsModel(QObject* parent = nullptr);
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    void clear();
    void setResults(std::vector<gitbolt::core::CommitData> results);
};

} // namespace gitbolt::models
