#include "models/CommitLogModel.h"
#include "util/PerformanceTimer.h"
#include <QDateTime>
#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace gitbolt::models {

CommitLogModel::CommitLogModel(QObject* parent) : QAbstractTableModel(parent) {}

int CommitLogModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    return static_cast<int>(commits_.size());
}

int CommitLogModel::columnCount(const QModelIndex&) const {
    return static_cast<int>(CommitLogColumn::Count);
}

QVariant CommitLogModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= static_cast<int>(commits_.size())) return {};
    const auto& commit = commits_[static_cast<size_t>(index.row())];

    if (role == GRAPH_DATA_ROLE || role == COMMIT_DATA_ROLE)
        return QVariant::fromValue(static_cast<quintptr>(index.row()));

    if (role == Qt::DisplayRole) {
        switch (static_cast<CommitLogColumn>(index.column())) {
            case CommitLogColumn::Graph: return {};
            case CommitLogColumn::Message: return QString::fromStdString(commit.summary);
            case CommitLogColumn::Author: return QString::fromStdString(commit.author.name);
            case CommitLogColumn::Date: {
                auto t = std::chrono::system_clock::to_time_t(commit.author.when);
                return QDateTime::fromSecsSinceEpoch(static_cast<qint64>(t)).toString("yyyy-MM-dd hh:mm");
            }
            case CommitLogColumn::Hash: return QString::fromStdString(commit.id.toShortHex());
            default: return {};
        }
    }
    return {};
}

QVariant CommitLogModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    switch (static_cast<CommitLogColumn>(section)) {
        case CommitLogColumn::Graph:   return tr("Graph");
        case CommitLogColumn::Message: return tr("Message");
        case CommitLogColumn::Author:  return tr("Author");
        case CommitLogColumn::Date:    return tr("Date");
        case CommitLogColumn::Hash:    return tr("Hash");
        default: return {};
    }
}

bool CommitLogModel::canFetchMore(const QModelIndex& parent) const {
    return !parent.isValid() && hasMore_;
}

void CommitLogModel::fetchMore(const QModelIndex& parent) {
    if (parent.isValid()) return;
    emit requestMoreCommits(static_cast<int>(commits_.size()), PAGE_SIZE);
}

void CommitLogModel::setCommits(std::vector<git::CommitData> commits) {
    util::PerformanceTimer timer("CommitLogModel::setCommits");
    beginResetModel();
    commits_ = std::move(commits);
    hasMore_ = commits_.size() >= PAGE_SIZE;
    residentPages_.clear();
    for (size_t i = 0; i < commits_.size(); i += PAGE_SIZE)
        residentPages_.insert(static_cast<int>(i / PAGE_SIZE));
    computeGraphData();
    endResetModel();
}

void CommitLogModel::appendCommits(const std::vector<git::CommitData>& commits) {
    util::PerformanceTimer timer("CommitLogModel::appendCommits");
    if (commits.empty()) { hasMore_ = false; return; }
    int first = static_cast<int>(commits_.size());
    int last = first + static_cast<int>(commits.size()) - 1;
    beginInsertRows(QModelIndex(), first, last);
    commits_.insert(commits_.end(), commits.begin(), commits.end());
    hasMore_ = commits.size() >= PAGE_SIZE;
    // Register newly added pages
    for (int r = first; r <= last; r += PAGE_SIZE)
        residentPages_.insert(pageForRow(r));
    computeGraphData();
    endInsertRows();
    evictDistantPages();
}

void CommitLogModel::clear() {
    beginResetModel();
    commits_.clear();
    graphData_.clear();
    residentPages_.clear();
    hasMore_ = true;
    endResetModel();
}

const git::CommitData* CommitLogModel::commitAt(int row) const {
    if (row < 0 || static_cast<size_t>(row) >= commits_.size()) return nullptr;
    return &commits_[static_cast<size_t>(row)];
}

const GraphRowData* CommitLogModel::graphAt(int row) const {
    if (row < 0 || static_cast<size_t>(row) >= graphData_.size()) return nullptr;
    return &graphData_[static_cast<size_t>(row)];
}

// ---------------------------------------------------------------------------
// Sliding-window page cache
// ---------------------------------------------------------------------------

void CommitLogModel::setMaxCachedPages(int pages) {
    maxCachedPages_ = std::max(1, pages);
    evictDistantPages();
}

void CommitLogModel::setVisibleRange(int first, int last) {
    visibleFirst_ = first;
    visibleLast_ = last;
    evictDistantPages();
}

int CommitLogModel::pageForRow(int row) const {
    return row / PAGE_SIZE;
}

void CommitLogModel::evictDistantPages() {
    if (static_cast<int>(residentPages_.size()) <= maxCachedPages_) return;

    int centerPage = pageForRow((visibleFirst_ + visibleLast_) / 2);

    // Collect pages sorted by distance from center
    struct PageDist { int page; int dist; };
    std::vector<PageDist> pages;
    pages.reserve(residentPages_.size());
    for (int p : residentPages_)
        pages.push_back({p, std::abs(p - centerPage)});

    std::sort(pages.begin(), pages.end(),
              [](const PageDist& a, const PageDist& b) { return a.dist > b.dist; });

    // Evict farthest pages until within budget
    while (static_cast<int>(residentPages_.size()) > maxCachedPages_ && !pages.empty()) {
        int evictPage = pages.front().page;
        pages.erase(pages.begin());
        residentPages_.erase(evictPage);
        emit pageEvicted(evictPage * PAGE_SIZE);
    }
}

// ---------------------------------------------------------------------------
// Graph computation
// ---------------------------------------------------------------------------

void CommitLogModel::computeGraphData() {
    util::PerformanceTimer timer("CommitLogModel::computeGraphData");
    graphData_.clear();
    graphData_.resize(commits_.size());

    // ----- Lane assignment (greedy, one pass top-to-bottom) -----
    //
    // Each `activeLanes[i]` holds the OID of the commit currently
    // expected on lane `i` (zero = free slot). Parallel to it,
    // `laneColors[i]` carries the COLOR INDEX assigned to that lane,
    // so the same branch line keeps a stable color from tip to root
    // even though the commits on it have unrelated SHAs. Without
    // this the lane would change color at every commit, producing a
    // rainbow mess instead of a recognisable branch.
    //
    // Color allocation: we use a monotonic counter (mod 16) so each
    // newly-created lane (or reused free slot) gets the next unused
    // color in the cycle. Lanes never re-inherit a freed slot's old
    // color — that'd defeat the visual purpose of the recycle.
    std::vector<git::ObjectId> activeLanes;
    std::vector<int> laneColors;     // parallel to activeLanes
    std::unordered_map<git::ObjectId, int, git::ObjectId::Hash> oidToLane;

    int nextColorCounter = 0;
    auto allocColor = [&]() {
        int c = nextColorCounter % 16;
        ++nextColorCounter;
        return c;
    };

    // Allocate (or reuse) a lane slot. Returns the slot index. Always
    // assigns it a fresh color (caller must NOT call this for the
    // first-parent inheritance case where the lane keeps its color).
    auto allocLane = [&](int forbidLane = -1) {
        for (int i = 0; i < static_cast<int>(activeLanes.size()); ++i) {
            if (activeLanes[static_cast<size_t>(i)].isZero() && i != forbidLane) {
                laneColors[static_cast<size_t>(i)] = allocColor();
                return i;
            }
        }
        activeLanes.push_back(git::ObjectId());
        laneColors.push_back(allocColor());
        return static_cast<int>(activeLanes.size()) - 1;
    };

    for (size_t row = 0; row < commits_.size(); ++row) {
        const auto& commit = commits_[row];
        auto& rowData = graphData_[row];

        // ---- Step 1: figure out which lane this commit lives on ----
        // If a previously-walked child already reserved a lane for
        // this commit (because we tracked it as a parent of that
        // child), reuse it — and inherit that lane's color, which is
        // also the color of the line coming down from above.
        // Otherwise this commit is the first time we see its hash,
        // which means it's a branch tip in the visible window — give
        // it a fresh lane.
        int  commitLane = -1;
        bool hasIncomingLane = false;
        auto it = oidToLane.find(commit.id);
        if (it != oidToLane.end()) {
            commitLane = it->second;
            oidToLane.erase(it);
            hasIncomingLane = true;
        } else {
            commitLane = allocLane();
        }

        rowData.commitLane = commitLane;
        rowData.colorIndex = laneColors[static_cast<size_t>(commitLane)];

        // ---- Step 2: pass-through verticals for OTHER active lanes ----
        // Every lane that's still tracking a future commit needs a
        // full top-to-bottom vertical line on this row. The commit's
        // own lane is handled separately (top half + bottom half) so
        // it shows the dot crisply in the middle.
        for (int i = 0; i < static_cast<int>(activeLanes.size()); ++i) {
            if (i == commitLane) continue;
            if (!activeLanes[static_cast<size_t>(i)].isZero()) {
                rowData.segments.push_back({i, i, LaneSegmentType::PassThrough,
                    laneColors[static_cast<size_t>(i)]});
            }
        }

        // ---- Step 3: top half of THIS commit's lane ----
        // If the lane was already coming down from above (a child
        // reserved it), draw the top half so the dot connects to
        // the lane line coming in. Without this segment the dot
        // appears to float, disconnected from the history above.
        if (hasIncomingLane) {
            rowData.segments.push_back({commitLane, commitLane,
                LaneSegmentType::End, rowData.colorIndex});
        }

        // Free this commit's lane slot — the first parent (if any)
        // will reclaim it below, otherwise it becomes available for
        // a future branch tip.
        activeLanes[static_cast<size_t>(commitLane)] = git::ObjectId();

        // ---- Step 4: bottom-half segments for each parent ----
        // - First parent (p == 0) inherits the commit's lane and its
        //   color. This is the "main branch continues straight down"
        //   case — most commits are like this.
        // - Subsequent parents are merge sources; each gets its own
        //   new lane (or an already-in-flight lane if we've seen the
        //   parent before from a different child).
        for (size_t p = 0; p < commit.parentIds.size(); ++p) {
            const auto& parentId = commit.parentIds[p];
            auto parentIt = oidToLane.find(parentId);

            if (parentIt != oidToLane.end()) {
                // Parent is already on an existing lane (because some
                // earlier commit also has it as a parent). Draw a
                // merge curve from this commit's lane down to that
                // existing lane, in the destination lane's color.
                const int destLane = parentIt->second;
                rowData.segments.push_back({commitLane, destLane,
                    LaneSegmentType::MergeRight,
                    laneColors[static_cast<size_t>(destLane)]});
            } else if (p == 0) {
                // First parent stays on this commit's lane.
                activeLanes[static_cast<size_t>(commitLane)] = parentId;
                oidToLane[parentId] = commitLane;
                rowData.segments.push_back({commitLane, commitLane,
                    LaneSegmentType::Start, rowData.colorIndex});
            } else {
                // Merge source — split off into a new lane to the
                // right (or reuse a free slot, but never the commit's
                // own lane). New lane gets a fresh color.
                const int newLane = allocLane(/*forbidLane=*/commitLane);
                activeLanes[static_cast<size_t>(newLane)] = parentId;
                oidToLane[parentId] = newLane;
                rowData.segments.push_back({commitLane, newLane,
                    LaneSegmentType::SplitRight,
                    laneColors[static_cast<size_t>(newLane)]});
            }
        }

        rowData.maxLane = commitLane;
        for (const auto& seg : rowData.segments)
            rowData.maxLane = std::max(rowData.maxLane,
                                       std::max(seg.fromLane, seg.toLane));
    }
}

} // namespace gitbolt::models
