#pragma once

#include "git/Commit.h"
#include "git/ObjectId.h"

#include <QAbstractTableModel>
#include <unordered_set>
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

    void setCommits(std::vector<git::CommitData> commits);
    void appendCommits(const std::vector<git::CommitData>& commits);
    void clear();

    const git::CommitData* commitAt(int row) const;
    const GraphRowData* graphAt(int row) const;

    // Sliding-window page cache management
    void setMaxCachedPages(int pages);
    int maxCachedPages() const { return maxCachedPages_; }
    void setVisibleRange(int first, int last);

signals:
    void requestMoreCommits(int offset, int count);
    void pageEvicted(int offset);

private:
    void computeGraphData();
    void evictDistantPages();
    int pageForRow(int row) const;

    std::vector<git::CommitData> commits_;
    std::vector<GraphRowData> graphData_;
    bool hasMore_ = true;

    // Sliding-window cache state
    int maxCachedPages_ = 20;    // default: 20 pages = 5120 rows
    int visibleFirst_ = 0;
    int visibleLast_ = 0;
    std::unordered_set<int> residentPages_;
};

} // namespace gitbolt::models
