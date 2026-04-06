#include "models/FileStatusModel.h"

#include <QFileInfo>
#include <QPainter>
#include <QPixmap>

namespace gitbolt::models {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

FileStatusModel::FileStatusModel(QObject* parent)
    : QAbstractTableModel(parent)
{
}

// ---------------------------------------------------------------------------
// QAbstractTableModel interface
// ---------------------------------------------------------------------------

int FileStatusModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid())
        return 0;
    return static_cast<int>(visible_.size());
}

int FileStatusModel::columnCount(const QModelIndex& parent) const
{
    if (parent.isValid())
        return 0;
    return ColumnCount;
}

QVariant FileStatusModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0
        || index.row() >= static_cast<int>(visible_.size()))
        return {};

    const auto& entry = *visible_[static_cast<size_t>(index.row())];
    const int col = index.column();

    switch (role) {
    case Qt::DisplayRole: {
        switch (col) {
        case StatusIcon:
            return QString(statusLetter(entry, stagedFilter_));
        case FileName: {
            const QString fullPath = QString::fromStdString(entry.path);
            return QFileInfo(fullPath).fileName();
        }
        case Path: {
            // Prefix the path with the single-letter status so that
            // single-column list views (StagingWidget) show both the
            // status and the file without needing a delegate.
            const QChar letter = statusLetter(entry, stagedFilter_);
            const QString path = QString::fromStdString(entry.path);
            return QStringLiteral("%1  %2").arg(letter).arg(path);
        }
        default:
            break;
        }
        break;
    }
    case Qt::ForegroundRole: {
        // Foreground tint on the status letter column AND on the path
        // column, so a single-column QListView (like StagingWidget uses)
        // still shows status colour without needing a custom delegate.
        if (col == StatusIcon || col == Path)
            return statusColor(entry, stagedFilter_);
        break;
    }
    case Qt::DecorationRole: {
        // Colored-dot icon on the status letter column AND on the path
        // column, for the same reason as above. A QListView set to
        // modelColumn(Path) picks up this icon without extra work.
        if (col == StatusIcon || col == Path)
            return statusIcon(entry, stagedFilter_);
        break;
    }
    case Qt::ToolTipRole: {
        return QString::fromStdString(entry.path);
    }
    default:
        break;
    }

    return {};
}

QVariant FileStatusModel::headerData(int section, Qt::Orientation orientation,
                                     int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};

    switch (section) {
    case StatusIcon: return tr("S");
    case FileName:   return tr("File");
    case Path:       return tr("Path");
    default:         break;
    }
    return {};
}

// ---------------------------------------------------------------------------
// Data management
// ---------------------------------------------------------------------------

void FileStatusModel::setEntries(std::vector<gitbolt::git::StatusEntry> entries)
{
    beginResetModel();
    allEntries_ = std::move(entries);
    rebuildVisible();
    endResetModel();
}

void FileStatusModel::clear()
{
    beginResetModel();
    allEntries_.clear();
    visible_.clear();
    endResetModel();
}

const gitbolt::git::StatusEntry* FileStatusModel::entryAt(int row) const
{
    if (row < 0 || row >= static_cast<int>(visible_.size()))
        return nullptr;
    return visible_[static_cast<size_t>(row)];
}

void FileStatusModel::setStagedFilter(bool staged)
{
    if (stagedFilter_ == staged)
        return;
    beginResetModel();
    stagedFilter_ = staged;
    rebuildVisible();
    endResetModel();
}

QString FileStatusModel::pathAt(int row) const
{
    const auto* e = entryAt(row);
    if (!e)
        return {};
    return QString::fromStdString(e->path);
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

void FileStatusModel::rebuildVisible()
{
    visible_.clear();
    for (const auto& entry : allEntries_) {
        if (stagedFilter_ && entry.isStaged())
            visible_.push_back(&entry);
        else if (!stagedFilter_ && entry.isWorkingTree())
            visible_.push_back(&entry);
    }
}

QChar FileStatusModel::statusLetter(const gitbolt::git::StatusEntry& entry,
                                    bool staged)
{
    using FS = gitbolt::git::FileStatus;

    if (staged) {
        if (hasFlag(entry.status, FS::IndexNew))        return QLatin1Char('A');
        if (hasFlag(entry.status, FS::IndexModified))    return QLatin1Char('M');
        if (hasFlag(entry.status, FS::IndexDeleted))     return QLatin1Char('D');
        if (hasFlag(entry.status, FS::IndexRenamed))     return QLatin1Char('R');
        if (hasFlag(entry.status, FS::IndexTypeChange))  return QLatin1Char('T');
    } else {
        if (hasFlag(entry.status, FS::WtNew))            return QLatin1Char('?');
        if (hasFlag(entry.status, FS::WtModified))       return QLatin1Char('M');
        if (hasFlag(entry.status, FS::WtDeleted))        return QLatin1Char('D');
        if (hasFlag(entry.status, FS::WtRenamed))        return QLatin1Char('R');
        if (hasFlag(entry.status, FS::WtTypeChange))     return QLatin1Char('T');
        if (hasFlag(entry.status, FS::WtUnreadable))     return QLatin1Char('!');
    }
    if (hasFlag(entry.status, FS::Conflicted))
        return QLatin1Char('C');

    return QLatin1Char(' ');
}

QColor FileStatusModel::statusColor(const gitbolt::git::StatusEntry& entry,
                                    bool staged)
{
    using FS = gitbolt::git::FileStatus;

    if (hasFlag(entry.status, FS::Conflicted))
        return QColor(204, 0, 0);   // red

    if (staged) {
        if (hasFlag(entry.status, FS::IndexNew))        return QColor(0, 170, 0);    // green
        if (hasFlag(entry.status, FS::IndexDeleted))     return QColor(204, 0, 0);    // red
        return QColor(0, 102, 204); // blue
    }

    if (hasFlag(entry.status, FS::WtNew))       return QColor(128, 128, 128); // gray
    if (hasFlag(entry.status, FS::WtDeleted))    return QColor(204, 0, 0);
    return QColor(0, 102, 204);
}

QIcon FileStatusModel::statusIcon(const gitbolt::git::StatusEntry& entry,
                                  bool staged)
{
    const int sz = 12;
    QPixmap pix(sz, sz);
    pix.fill(Qt::transparent);
    QPainter p(&pix);
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(statusColor(entry, staged));
    p.setPen(Qt::NoPen);
    p.drawEllipse(1, 1, sz - 2, sz - 2);
    p.end();
    return QIcon(pix);
}

} // namespace gitbolt::models
