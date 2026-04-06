#include "models/BlameModel.h"

#include <QColor>
#include <QDateTime>
#include <QFont>
#include <QIcon>
#include <chrono>

namespace gitbolt::models {

// Alternating hunk colors for visual distinction
static const QColor kHunkColors[] = {
    QColor(240, 248, 255, 40),   // alice blue tint
    QColor(255, 250, 240, 40),   // floral white tint
    QColor(245, 255, 250, 40),   // mint cream tint
    QColor(255, 245, 238, 40),   // seashell tint
    QColor(248, 248, 255, 40),   // ghost white tint
    QColor(255, 255, 240, 40),   // ivory tint
};
static constexpr int kHunkColorCount = sizeof(kHunkColors) / sizeof(kHunkColors[0]);

BlameModel::BlameModel(QObject* parent)
    : QAbstractTableModel(parent) {}

int BlameModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid())
        return 0;
    return static_cast<int>(lines_.size());
}

int BlameModel::columnCount(const QModelIndex& parent) const {
    if (parent.isValid())
        return 0;
    return ColumnCount;
}

QVariant BlameModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid())
        return {};

    int row = index.row();
    if (row < 0 || row >= static_cast<int>(lines_.size()))
        return {};

    const auto& line = lines_[row];

    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case ColCommit:
            return QString::fromStdString(line.commitId.toShortHex());
        case ColAuthor:
            return line.author;
        case ColDate:
            return line.date;
        case ColLine:
            return line.lineNumber;
        case ColContent:
            return line.content;
        default:
            return {};
        }
    }

    if (role == Qt::BackgroundRole) {
        return hunkBackgroundColor(line.hunkIndex);
    }

    if (role == Qt::TextAlignmentRole) {
        if (index.column() == ColLine)
            return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
        return static_cast<int>(Qt::AlignLeft | Qt::AlignVCenter);
    }

    if (role == Qt::FontRole && index.column() == ColContent) {
        QFont mono(QStringLiteral("Monospace"));
        mono.setStyleHint(QFont::Monospace);
        return mono;
    }

    if (role == CommitIdRole)
        return QString::fromStdString(line.commitId.toHex());

    if (role == HunkIndexRole)
        return line.hunkIndex;

    return {};
}

QVariant BlameModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};

    switch (section) {
    case ColCommit:  return tr("Commit");
    case ColAuthor:  return tr("Author");
    case ColDate:    return tr("Date");
    case ColLine:    return tr("Line");
    case ColContent: return tr("Content");
    default:         return {};
    }
}

void BlameModel::setBlameResult(git::BlameResult result) {
    beginResetModel();
    blameResult_ = std::move(result);
    filePath_ = QString::fromStdString(blameResult_.path);
    flattenHunks();
    endResetModel();
}

void BlameModel::clear() {
    beginResetModel();
    blameResult_ = {};
    lines_.clear();
    filePath_.clear();
    endResetModel();
}

gitbolt::git::ObjectId BlameModel::commitIdAtRow(int row) const {
    if (row >= 0 && row < static_cast<int>(lines_.size()))
        return lines_[row].commitId;
    return {};
}

void BlameModel::flattenHunks() {
    lines_.clear();

    // We have blameResult_.hunks (ranges) and blameResult_.lines (full file content).
    // Each hunk specifies startLine (1-based) and lineCount.
    // Map each line of the file to its blame hunk.

    // Build a lookup: lineNumber (1-based) -> hunk index
    const auto& hunks = blameResult_.hunks;
    const auto& fileLines = blameResult_.lines;

    // Create a mapping from 1-based line number to hunk index
    std::vector<int> lineToHunk(fileLines.size() + 1, -1);
    for (int h = 0; h < static_cast<int>(hunks.size()); ++h) {
        uint32_t start = hunks[h].startLine;
        uint32_t count = hunks[h].lineCount;
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t ln = start + i;
            if (ln > 0 && ln <= fileLines.size())
                lineToHunk[ln] = h;
        }
    }

    lines_.reserve(fileLines.size());
    for (size_t i = 0; i < fileLines.size(); ++i) {
        int lineNum = static_cast<int>(i + 1);
        int hIdx = (lineNum < static_cast<int>(lineToHunk.size()))
                       ? lineToHunk[lineNum]
                       : -1;

        BlameLine bl;
        bl.lineNumber = lineNum;
        bl.content = QString::fromStdString(fileLines[i]);
        bl.hunkIndex = hIdx;

        if (hIdx >= 0 && hIdx < static_cast<int>(hunks.size())) {
            const auto& hunk = hunks[hIdx];
            bl.commitId = hunk.commitId;
            bl.author = QString::fromStdString(hunk.signature.name);

            // Format date as relative
            auto now = std::chrono::system_clock::now();
            auto commitTime = hunk.signature.when;
            auto diff = std::chrono::duration_cast<std::chrono::hours>(now - commitTime);
            int hours = static_cast<int>(diff.count());

            if (hours < 1)
                bl.date = tr("just now");
            else if (hours < 24)
                bl.date = tr("%1 hours ago").arg(hours);
            else if (hours < 24 * 30)
                bl.date = tr("%1 days ago").arg(hours / 24);
            else if (hours < 24 * 365)
                bl.date = tr("%1 months ago").arg(hours / (24 * 30));
            else
                bl.date = tr("%1 years ago").arg(hours / (24 * 365));
        }

        lines_.push_back(std::move(bl));
    }
}

QColor BlameModel::hunkBackgroundColor(int hunkIndex) const {
    if (hunkIndex < 0)
        return {};
    return kHunkColors[hunkIndex % kHunkColorCount];
}

} // namespace gitbolt::models
