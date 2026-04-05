#pragma once

#include "git/Stash.h"

#include <QAbstractTableModel>
#include <vector>

namespace gitbolt::models {

class StashModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column {
        ColIndex = 0,
        ColMessage,
        ColDate,
        ColumnCount,
    };

    enum Roles {
        StashIndexRole = Qt::UserRole + 1,
        ObjectIdRole,
    };

    explicit StashModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

    void setStashes(std::vector<gitbolt::git::StashEntry> stashes);
    void clear();

    size_t stashIndexAtRow(int row) const;

private:
    std::vector<gitbolt::git::StashEntry> stashes_;
};

} // namespace gitbolt::models
