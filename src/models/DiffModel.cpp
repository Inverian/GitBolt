#include "models/DiffModel.h"

#include <QColor>
#include <QFileInfo>

namespace gitbolt::models {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

DiffModel::DiffModel(QObject* parent)
    : QAbstractTableModel(parent)
{
}

// ---------------------------------------------------------------------------
// QAbstractTableModel interface
// ---------------------------------------------------------------------------

int DiffModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid())
        return 0;
    return static_cast<int>(result_.files.size());
}

int DiffModel::columnCount(const QModelIndex& parent) const
{
    if (parent.isValid())
        return 0;
    return ColumnCount;
}

QVariant DiffModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0
        || index.row() >= static_cast<int>(result_.files.size()))
        return {};

    const auto& file = result_.files[static_cast<size_t>(index.row())];

    switch (role) {
    case Qt::DisplayRole: {
        switch (index.column()) {
        case Status:
            return QString(statusLetter(file.status));
        case FileName: {
            const QString path = QString::fromStdString(file.path());
            return QFileInfo(path).fileName();
        }
        default:
            break;
        }
        break;
    }
    case Qt::ForegroundRole: {
        if (index.column() == Status)
            return statusColor(file.status);
        break;
    }
    case Qt::ToolTipRole: {
        return QString::fromStdString(file.path());
    }
    default:
        break;
    }

    return {};
}

QVariant DiffModel::headerData(int section, Qt::Orientation orientation,
                               int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};

    switch (section) {
    case Status:   return tr("S");
    case FileName: return tr("File");
    default:       break;
    }
    return {};
}

// ---------------------------------------------------------------------------
// Data management
// ---------------------------------------------------------------------------

void DiffModel::setDiffResult(gitbolt::git::DiffResult result)
{
    beginResetModel();
    result_ = std::move(result);
    endResetModel();
}

void DiffModel::clear()
{
    beginResetModel();
    result_ = {};
    endResetModel();
}

const gitbolt::git::DiffFileEntry* DiffModel::fileAt(int row) const
{
    if (row < 0 || row >= static_cast<int>(result_.files.size()))
        return nullptr;
    return &result_.files[static_cast<size_t>(row)];
}

int DiffModel::fileCount() const
{
    return static_cast<int>(result_.files.size());
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

QChar DiffModel::statusLetter(gitbolt::git::DiffStatus s)
{
    using DS = gitbolt::git::DiffStatus;
    switch (s) {
    case DS::Added:       return QLatin1Char('A');
    case DS::Deleted:     return QLatin1Char('D');
    case DS::Modified:    return QLatin1Char('M');
    case DS::Renamed:     return QLatin1Char('R');
    case DS::Copied:      return QLatin1Char('C');
    case DS::Untracked:   return QLatin1Char('?');
    case DS::Ignored:     return QLatin1Char('!');
    case DS::TypeChanged: return QLatin1Char('T');
    case DS::Conflicted:  return QLatin1Char('U');
    default:              return QLatin1Char(' ');
    }
}

QColor DiffModel::statusColor(gitbolt::git::DiffStatus s)
{
    using DS = gitbolt::git::DiffStatus;
    switch (s) {
    case DS::Added:      return QColor(0, 170, 0);
    case DS::Deleted:    return QColor(204, 0, 0);
    case DS::Modified:   return QColor(0, 102, 204);
    case DS::Renamed:    return QColor(0, 102, 204);
    case DS::Conflicted: return QColor(204, 0, 0);
    case DS::Untracked:  return QColor(128, 128, 128);
    default:             return QColor(100, 100, 100);
    }
}

} // namespace gitbolt::models
