#pragma once

#include "git/Repository.h"

#include <QAbstractTableModel>
#include <QAction>
#include "widgets/RecordTablePanel.h"
#include <QWidget>
#include <vector>

namespace gitbolt::widgets {

// ---------------------------------------------------------------------------
// Internal table model for worktree data
// ---------------------------------------------------------------------------

class WorktreeTableModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column {
        ColName = 0,
        ColPath,
        ColBranch,
        ColLocked,
        ColumnCount,
    };

    enum Roles {
        WorktreeNameRole = Qt::UserRole + 1,
        WorktreePathRole,
        WorktreeBranchRole,
        WorktreeLockedRole,
    };

    explicit WorktreeTableModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

    void setWorktrees(std::vector<gitbolt::git::WorktreeInfo> worktrees);
    void clear();

    QString nameAtRow(int row) const;
    QString pathAtRow(int row) const;
    bool isLockedAtRow(int row) const;

private:
    std::vector<gitbolt::git::WorktreeInfo> worktrees_;
};

// ---------------------------------------------------------------------------
// WorktreeWidget
// ---------------------------------------------------------------------------

class WorktreeWidget : public RecordTablePanel {
    Q_OBJECT
public:
    explicit WorktreeWidget(QWidget* parent = nullptr);

    void setWorktrees(std::vector<gitbolt::git::WorktreeInfo> worktrees);
    void clear();

signals:
    void addRequested();
    void removeRequested(const QString& name);
    void lockRequested(const QString& name);
    void unlockRequested(const QString& name);
    void openRequested(const QString& path);

private slots:
    void onContextMenu(const QPoint& pos);
    void onRemoveClicked();
    void onLockUnlockClicked();

private:
    void setupUI();
    QString selectedWorktreeName() const;
    QString selectedWorktreePath() const;
    bool selectedWorktreeLocked() const;

    WorktreeTableModel* model_ = nullptr;
    QAction* addAction_ = nullptr;
    QAction* removeAction_ = nullptr;
    QAction* lockUnlockAction_ = nullptr;
};

} // namespace gitbolt::widgets
