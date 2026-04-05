#pragma once

#include "git/ObjectId.h"

#include <QAbstractTableModel>
#include <chrono>
#include <string>
#include <vector>

namespace gitbolt::models {

struct ReflogEntry {
    std::string action;     // e.g., "commit", "checkout", "merge", "rebase"
    std::string message;    // full reflog message
    gitbolt::git::ObjectId id;
    std::chrono::system_clock::time_point timestamp;
};

class ReflogModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column {
        ColAction = 0,
        ColHash,
        ColMessage,
        ColDate,
        ColumnCount,
    };

    enum Roles {
        ObjectIdRole = Qt::UserRole + 1,
        ActionRole,
        FullMessageRole,
    };

    explicit ReflogModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

    void setEntries(std::vector<ReflogEntry> entries);
    void clear();

    gitbolt::git::ObjectId objectIdAtRow(int row) const;

private:
    std::vector<ReflogEntry> entries_;
};

} // namespace gitbolt::models
