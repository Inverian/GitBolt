#pragma once

#include "git/Status.h"

#include <QAbstractTableModel>
#include <QColor>
#include <QIcon>
#include <vector>

namespace gitbolt::models {

class FileStatusModel : public QAbstractTableModel {
    Q_OBJECT

public:
    enum Column {
        StatusIcon = 0,
        FileName,
        Path,
        ColumnCount
    };

    explicit FileStatusModel(QObject* parent = nullptr);

    // QAbstractTableModel interface
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

    // Data management
    void setEntries(std::vector<gitbolt::git::StatusEntry> entries);
    void clear();

    // Access a single entry by visible row
    const gitbolt::git::StatusEntry* entryAt(int row) const;

    // When true only staged entries are shown; when false only unstaged entries
    void setStagedFilter(bool staged);
    bool stagedFilter() const { return stagedFilter_; }

    // Convenience: path at a given visible row
    QString pathAt(int row) const;

private:
    void rebuildVisible();

    static QChar statusLetter(const gitbolt::git::StatusEntry& entry, bool staged);
    static QColor statusColor(const gitbolt::git::StatusEntry& entry, bool staged);
    static QIcon  statusIcon(const gitbolt::git::StatusEntry& entry, bool staged);

    std::vector<gitbolt::git::StatusEntry> allEntries_;
    std::vector<const gitbolt::git::StatusEntry*> visible_;
    bool stagedFilter_ = false;
};

} // namespace gitbolt::models
