#pragma once

#include "git/Commit.h"
#include "git/ObjectId.h"

#include <QAbstractTableModel>
#include <unordered_map>
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
    /// Append a page of commits. `offset` is the row the page was
    /// requested for; pass it through from logReady so a duplicate
    /// or out-of-order page (the same offset requested twice while
    /// the first request was still walking) is dropped instead of
    /// appended twice. -1 skips the check (tests / non-paged use).
    void appendCommits(const std::vector<git::CommitData>& commits,
                       int offset = -1);
    void clear();

    const git::CommitData* commitAt(int row) const;
    const GraphRowData* graphAt(int row) const;

    /// Toggle between absolute "yyyy-MM-dd hh:mm" formatting and
    /// relative "X minutes/hours/days ago" formatting in the Date
    /// column. Forwards a dataChanged signal for the column so the
    /// view repaints.
    void setRelativeDate(bool relative);
    bool relativeDate() const { return relativeDate_; }

    /// Toggle whether the Date column shows the author date (when
    /// the commit was originally written) or the committer date
    /// (when it was last rewritten via rebase / amend). Default
    /// is author date — matches `git log`'s default and is what
    /// GitExtensions shows. Emits dataChanged for the Date column.
    void setUseAuthorDate(bool useAuthor);
    bool useAuthorDate() const { return useAuthorDate_; }

    /// Toggle whether the Message column shows just the commit
    /// summary (first line) or the full body. When on and the
    /// body is non-empty, the cell renders "summary\n\nbody"
    /// — Qt expands the row height accordingly when the table
    /// has wordWrap enabled.
    void setShowMessageBody(bool showBody);
    bool showMessageBody() const { return showMessageBody_; }

signals:
    void requestMoreCommits(int offset, int count);

private:
    void computeGraphData();
    /// Continue the lane fold from the last computed row (used by
    /// appendCommits — O(new rows), not O(all rows)).
    void appendGraphRows();

    std::vector<git::CommitData> commits_;
    std::vector<GraphRowData> graphData_;
    bool hasMore_ = true;

    // ---- Lane-assignment checkpoint ----
    // Working state of the graph fold, carried across appendGraphRows
    // calls so appended pages continue from the last computed row.
    // Reset by setCommits/clear (via computeGraphData).
    std::vector<git::ObjectId> activeLanes_;
    std::vector<int> laneColors_;   // parallel to activeLanes_
    std::unordered_map<git::ObjectId, int, git::ObjectId::Hash> oidToLane_;
    int nextColorCounter_ = 0;
    // True while a requestMoreCommits round-trip is in flight. Qt
    // re-invokes fetchMore every time the view hits the bottom —
    // without this guard the same offset gets requested repeatedly
    // and the same 256 commits land more than once.
    bool fetchPending_ = false;

    // When true, the Date column renders "X ago" instead of an
    // absolute timestamp. Persisted by MainWindow via QSettings.
    bool relativeDate_ = false;
    // When true, Date column reflects the author timestamp; when
    // false, it shows the committer timestamp.
    bool useAuthorDate_ = true;
    // When true, Message column renders summary + full body
    // (separated by a blank line). Default is summary only.
    bool showMessageBody_ = false;
};

} // namespace gitbolt::models
