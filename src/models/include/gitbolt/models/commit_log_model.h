#pragma once

#include "gitbolt/core/commit.h"
#include "gitbolt/core/object_id.h"

#include <QAbstractTableModel>
#include <vector>

namespace gitbolt::models {

enum class LaneSegmentType { PassThrough, Start, End, MergeLeft, MergeRight, SplitLeft, SplitRight };

struct LaneSegment {
    int fromLane;
    int toLane;
    LaneSegmentType type;
    int colorIndex;
};

struct GraphRowData {
    int commitLane = 0;
    int colorIndex = 0;
    std::vector<LaneSegment> segments;
    int maxLane = 0;
};

enum class CommitLogColumn { Graph = 0, Message, Author, Date, Hash, Count };

class CommitLogModel : public QAbstractTableModel {
    Q_OBJECT
public:
    static constexpr int PAGE_SIZE = 256;
    static constexpr int GRAPH_DATA_ROLE = Qt::UserRole + 1;
    static constexpr int COMMIT_DATA_ROLE = Qt::UserRole + 2;

    explicit CommitLogModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

    bool canFetchMore(const QModelIndex& parent) const override;
    void fetchMore(const QModelIndex& parent) override;

    void setCommits(std::vector<core::CommitData> commits);
    void appendCommits(const std::vector<core::CommitData>& commits);
    void clear();

    const core::CommitData* commitAt(int row) const;
    const GraphRowData* graphAt(int row) const;

signals:
    void requestMoreCommits(int offset, int count);

private:
    void computeGraphData();

    std::vector<core::CommitData> commits_;
    std::vector<GraphRowData> graphData_;
    bool hasMore_ = true;
};

} // namespace gitbolt::models
