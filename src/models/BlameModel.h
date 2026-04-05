#pragma once

#include "git/Blame.h"

#include <QAbstractTableModel>
#include <QColor>
#include <vector>

namespace gitbolt::models {

// Expanded per-line blame data, flattened from BlameHunks
struct BlameLine {
    gitbolt::git::ObjectId commitId;
    QString author;
    QString date;
    int lineNumber;           // 1-based
    QString content;
    int hunkIndex;            // which hunk this line belongs to
};

class BlameModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column {
        ColCommit = 0,
        ColAuthor,
        ColDate,
        ColLine,
        ColContent,
        ColumnCount,
    };

    enum Roles {
        CommitIdRole = Qt::UserRole + 1,
        HunkIndexRole,
    };

    explicit BlameModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

    void setBlameResult(gitbolt::git::BlameResult result);
    void clear();

    gitbolt::git::ObjectId commitIdAtRow(int row) const;
    QString filePathDisplayed() const { return filePath_; }

private:
    void flattenHunks();
    QColor hunkBackgroundColor(int hunkIndex) const;

    gitbolt::git::BlameResult blameResult_;
    std::vector<BlameLine> lines_;
    QString filePath_;
};

} // namespace gitbolt::models
