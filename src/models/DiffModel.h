#pragma once

#include "git/Diff.h"

#include <QAbstractTableModel>
#include <vector>

namespace gitbolt::models {

class DiffModel : public QAbstractTableModel {
    Q_OBJECT

public:
    enum Column {
        Status = 0,
        FileName,
        ColumnCount
    };

    explicit DiffModel(QObject* parent = nullptr);

    // QAbstractTableModel interface
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

    // Data management
    void setDiffResult(gitbolt::git::DiffResult result);
    void clear();

    // Access
    const gitbolt::git::DiffResult& diffResult() const { return result_; }
    const gitbolt::git::DiffFileEntry* fileAt(int row) const;
    int fileCount() const;

private:
    static QChar statusLetter(gitbolt::git::DiffStatus s);
    static QColor statusColor(gitbolt::git::DiffStatus s);

    gitbolt::git::DiffResult result_;
};

} // namespace gitbolt::models
