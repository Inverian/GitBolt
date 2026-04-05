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

void CommitLogModel::setCommits(std::vector<core::CommitData> commits) {
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

void CommitLogModel::appendCommits(const std::vector<core::CommitData>& commits) {
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

const core::CommitData* CommitLogModel::commitAt(int row) const {
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

    std::vector<core::ObjectId> activeLanes;
    std::unordered_map<core::ObjectId, int, core::ObjectId::Hash> oidToLane;

    for (size_t row = 0; row < commits_.size(); ++row) {
        const auto& commit = commits_[row];
        auto& rowData = graphData_[row];

        // Find lane for this commit
        int commitLane = -1;
        auto it = oidToLane.find(commit.id);
        if (it != oidToLane.end()) {
            commitLane = it->second;
            oidToLane.erase(it);
        } else {
            commitLane = static_cast<int>(activeLanes.size());
            for (int i = 0; i < static_cast<int>(activeLanes.size()); ++i) {
                if (activeLanes[static_cast<size_t>(i)].isZero()) { commitLane = i; break; }
            }
            if (commitLane >= static_cast<int>(activeLanes.size()))
                activeLanes.push_back(core::ObjectId());
        }

        rowData.commitLane = commitLane;
        rowData.colorIndex = static_cast<int>(std::hash<std::string>{}(commit.id.toHex()) % 16);

        // Pass-through segments
        for (int i = 0; i < static_cast<int>(activeLanes.size()); ++i) {
            if (i == commitLane) continue;
            if (!activeLanes[static_cast<size_t>(i)].isZero()) {
                rowData.segments.push_back({i, i, LaneSegmentType::PassThrough,
                    static_cast<int>(std::hash<std::string>{}(activeLanes[static_cast<size_t>(i)].toHex()) % 16)});
            }
        }

        // Clear this lane
        if (commitLane < static_cast<int>(activeLanes.size()))
            activeLanes[static_cast<size_t>(commitLane)] = core::ObjectId();

        // Assign parents to lanes
        for (size_t p = 0; p < commit.parentIds.size(); ++p) {
            const auto& parentId = commit.parentIds[p];
            auto parentIt = oidToLane.find(parentId);

            if (parentIt != oidToLane.end()) {
                rowData.segments.push_back({commitLane, parentIt->second,
                    (p == 0) ? LaneSegmentType::Start : LaneSegmentType::MergeRight, rowData.colorIndex});
            } else {
                int parentLane;
                if (p == 0) {
                    parentLane = commitLane;
                } else {
                    parentLane = static_cast<int>(activeLanes.size());
                    for (int i = 0; i < static_cast<int>(activeLanes.size()); ++i) {
                        if (activeLanes[static_cast<size_t>(i)].isZero() && i != commitLane) { parentLane = i; break; }
                    }
                    if (parentLane >= static_cast<int>(activeLanes.size()))
                        activeLanes.push_back(core::ObjectId());
                }
                activeLanes[static_cast<size_t>(parentLane)] = parentId;
                oidToLane[parentId] = parentLane;
                rowData.segments.push_back({commitLane, parentLane,
                    (p == 0) ? LaneSegmentType::Start : LaneSegmentType::SplitRight, rowData.colorIndex});
            }
        }

        rowData.maxLane = commitLane;
        for (const auto& seg : rowData.segments)
            rowData.maxLane = std::max(rowData.maxLane, std::max(seg.fromLane, seg.toLane));
    }
}

} // namespace gitbolt::models
