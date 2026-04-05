#pragma once

#include "git/Commit.h"

#include <QAbstractTableModel>
#include <vector>

namespace gitbolt::models {

class SearchResultsModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column {
        ColHash = 0,
        ColMessage,
        ColAuthor,
        ColDate,
        ColumnCount,
    };

    enum Roles {
        CommitIdRole = Qt::UserRole + 1,
        FullMessageRole,
    };

    explicit SearchResultsModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

    void setResults(std::vector<gitbolt::git::CommitData> results);
    void clear();

    gitbolt::git::ObjectId commitIdAtRow(int row) const;
    int resultCount() const { return static_cast<int>(results_.size()); }

private:
    std::vector<gitbolt::git::CommitData> results_;
};

} // namespace gitbolt::models
