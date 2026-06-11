#include "widgets/WorktreeWidget.h"

#include <QApplication>
#include <QClipboard>
#include <QFont>
#include <QHeaderView>
#include <QMenu>
#include <QVBoxLayout>

namespace gitbolt::widgets {

// ===========================================================================
// WorktreeTableModel
// ===========================================================================

WorktreeTableModel::WorktreeTableModel(QObject* parent)
    : QAbstractTableModel(parent) {}

int WorktreeTableModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid())
        return 0;
    return static_cast<int>(worktrees_.size());
}

int WorktreeTableModel::columnCount(const QModelIndex& parent) const {
    if (parent.isValid())
        return 0;
    return ColumnCount;
}

QVariant WorktreeTableModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid())
        return {};

    const int row = index.row();
    if (row < 0 || row >= static_cast<int>(worktrees_.size()))
        return {};

    const auto& wt = worktrees_[row];

    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case ColName:   return QString::fromStdString(wt.name);
        case ColPath:   return QString::fromStdString(wt.path);
        case ColBranch: return QString::fromStdString(wt.branch);
        case ColLocked: return wt.isLocked ? tr("Locked") : tr("Unlocked");
        default:        return {};
        }
    }

    if (role == Qt::ToolTipRole) {
        return QStringLiteral("%1\nPath: %2\nBranch: %3\n%4")
            .arg(QString::fromStdString(wt.name))
            .arg(QString::fromStdString(wt.path))
            .arg(QString::fromStdString(wt.branch))
            .arg(wt.isLocked ? tr("Locked") : tr("Unlocked"));
    }

    if (role == Qt::DecorationRole && index.column() == ColLocked) {
        if (wt.isLocked)
            return QIcon::fromTheme(QStringLiteral("object-locked"));
        return QIcon::fromTheme(QStringLiteral("object-unlocked"));
    }

    if (role == Qt::ForegroundRole && index.column() == ColLocked) {
        if (wt.isLocked)
            return QColor(0xc6, 0x28, 0x28);   // red
        return QColor(0x2e, 0x7d, 0x32);       // green
    }

    if (role == Qt::FontRole && index.column() == ColPath) {
        QFont mono(QStringLiteral("Monospace"));
        mono.setStyleHint(QFont::Monospace);
        return mono;
    }

    // Custom roles
    if (role == WorktreeNameRole)
        return QString::fromStdString(wt.name);
    if (role == WorktreePathRole)
        return QString::fromStdString(wt.path);
    if (role == WorktreeBranchRole)
        return QString::fromStdString(wt.branch);
    if (role == WorktreeLockedRole)
        return wt.isLocked;

    return {};
}

QVariant WorktreeTableModel::headerData(int section, Qt::Orientation orientation,
                                         int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};

    switch (section) {
    case ColName:   return tr("Name");
    case ColPath:   return tr("Path");
    case ColBranch: return tr("Branch");
    case ColLocked: return tr("Status");
    default:        return {};
    }
}

void WorktreeTableModel::setWorktrees(std::vector<git::WorktreeInfo> worktrees) {
    beginResetModel();
    worktrees_ = std::move(worktrees);
    endResetModel();
}

void WorktreeTableModel::clear() {
    beginResetModel();
    worktrees_.clear();
    endResetModel();
}

QString WorktreeTableModel::nameAtRow(int row) const {
    if (row >= 0 && row < static_cast<int>(worktrees_.size()))
        return QString::fromStdString(worktrees_[row].name);
    return {};
}

QString WorktreeTableModel::pathAtRow(int row) const {
    if (row >= 0 && row < static_cast<int>(worktrees_.size()))
        return QString::fromStdString(worktrees_[row].path);
    return {};
}

bool WorktreeTableModel::isLockedAtRow(int row) const {
    if (row >= 0 && row < static_cast<int>(worktrees_.size()))
        return worktrees_[row].isLocked;
    return false;
}

// ===========================================================================
// WorktreeWidget
// ===========================================================================

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

WorktreeWidget::WorktreeWidget(QWidget* parent)
    : QWidget(parent)
    , tableView_(new QTableView(this))
    , model_(new WorktreeTableModel(this))
    , toolbar_(new QToolBar(this))
{
    setupUI();
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

void WorktreeWidget::setWorktrees(std::vector<git::WorktreeInfo> worktrees) {
    model_->setWorktrees(std::move(worktrees));
    tableView_->resizeColumnsToContents();
}

void WorktreeWidget::clear() {
    model_->clear();
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

QString WorktreeWidget::selectedWorktreeName() const {
    const auto indexes = tableView_->selectionModel()->selectedRows();
    if (indexes.isEmpty())
        return {};
    return model_->nameAtRow(indexes.first().row());
}

QString WorktreeWidget::selectedWorktreePath() const {
    const auto indexes = tableView_->selectionModel()->selectedRows();
    if (indexes.isEmpty())
        return {};
    return model_->pathAtRow(indexes.first().row());
}

bool WorktreeWidget::selectedWorktreeLocked() const {
    const auto indexes = tableView_->selectionModel()->selectedRows();
    if (indexes.isEmpty())
        return false;
    return model_->isLockedAtRow(indexes.first().row());
}

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------

void WorktreeWidget::onRemoveClicked() {
    const QString name = selectedWorktreeName();
    if (!name.isEmpty())
        emit removeRequested(name);
}

void WorktreeWidget::onLockUnlockClicked() {
    const QString name = selectedWorktreeName();
    if (name.isEmpty())
        return;

    if (selectedWorktreeLocked())
        emit unlockRequested(name);
    else
        emit lockRequested(name);
}

void WorktreeWidget::onContextMenu(const QPoint& pos) {
    const QModelIndex index = tableView_->indexAt(pos);
    if (!index.isValid())
        return;

    const int row = index.row();
    const QString name = model_->nameAtRow(row);
    const QString path = model_->pathAtRow(row);
    const bool locked  = model_->isLockedAtRow(row);

    if (name.isEmpty())
        return;

    QMenu menu(this);

    menu.addAction(tr("Open in New Window"), this, [this, path]() {
        emit openRequested(path);
    });

    menu.addSeparator();

    menu.addAction(tr("Remove"), this, [this, name]() {
        emit removeRequested(name);
    });

    if (locked) {
        menu.addAction(tr("Unlock"), this, [this, name]() {
            emit unlockRequested(name);
        });
    } else {
        menu.addAction(tr("Lock"), this, [this, name]() {
            emit lockRequested(name);
        });
    }

    menu.addSeparator();

    menu.addAction(tr("Copy Path"), this, [path]() {
        QApplication::clipboard()->setText(path);
    });

    menu.exec(tableView_->viewport()->mapToGlobal(pos));
}

// ---------------------------------------------------------------------------
// UI setup
// ---------------------------------------------------------------------------

void WorktreeWidget::setupUI() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // Toolbar
    toolbar_->setIconSize(QSize(16, 16));
    toolbar_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    addAction_ = toolbar_->addAction(
        QIcon::fromTheme(QStringLiteral("list-add")),
        tr("Add Worktree"));
    addAction_->setObjectName(QStringLiteral("worktree.add"));
    connect(addAction_, &QAction::triggered,
            this, &WorktreeWidget::addRequested);

    removeAction_ = toolbar_->addAction(
        QIcon::fromTheme(QStringLiteral("list-remove")),
        tr("Remove"));
    removeAction_->setObjectName(QStringLiteral("worktree.remove"));
    connect(removeAction_, &QAction::triggered,
            this, &WorktreeWidget::onRemoveClicked);

    lockUnlockAction_ = toolbar_->addAction(
        QIcon::fromTheme(QStringLiteral("object-locked")),
        tr("Lock/Unlock"));
    lockUnlockAction_->setObjectName(QStringLiteral("worktree.lockUnlock"));
    connect(lockUnlockAction_, &QAction::triggered,
            this, &WorktreeWidget::onLockUnlockClicked);

    layout->addWidget(toolbar_);

    // Table view
    tableView_->setModel(model_);
    tableView_->setAlternatingRowColors(true);
    tableView_->setSelectionBehavior(QAbstractItemView::SelectRows);
    tableView_->setSelectionMode(QAbstractItemView::SingleSelection);
    tableView_->setShowGrid(false);
    tableView_->setContextMenuPolicy(Qt::CustomContextMenu);
    tableView_->verticalHeader()->setVisible(false);
    tableView_->verticalHeader()->setDefaultSectionSize(22);

    auto* hdr = tableView_->horizontalHeader();
    hdr->setStretchLastSection(true);
    hdr->setSectionResizeMode(QHeaderView::ResizeToContents);

    layout->addWidget(tableView_, 1);

    // Connections
    connect(tableView_, &QTableView::customContextMenuRequested,
            this, &WorktreeWidget::onContextMenu);
}

} // namespace gitbolt::widgets
