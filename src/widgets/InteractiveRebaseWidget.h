#pragma once

#include "git/Commit.h"
#include "git/ObjectId.h"
#include "git/Rebase.h"

#include <QAbstractListModel>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QStyledItemDelegate>
#include <QToolBar>
#include <QWidget>

class QListView;

namespace gitbolt::widgets {

// ---------------------------------------------------------------------------
// RebaseListModel -- drives the QListView with drag/drop reorder support
// ---------------------------------------------------------------------------
class RebaseListModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Roles {
        OperationTypeRole = Qt::UserRole + 1,
        CommitHashRole,
        CommitMessageRole,
    };

    explicit RebaseListModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole) override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;

    // Drag-and-drop reorder
    Qt::DropActions supportedDropActions() const override;
    bool moveRows(const QModelIndex& sourceParent, int sourceRow, int count,
                  const QModelIndex& destinationParent, int destinationChild) override;

    // Mime support for internal drag-and-drop
    QStringList mimeTypes() const override;
    QMimeData* mimeData(const QModelIndexList& indexes) const override;
    bool dropMimeData(const QMimeData* data, Qt::DropAction action,
                      int row, int column, const QModelIndex& parent) override;

    // Populate / query
    void setOperations(std::vector<git::RebaseOperation> ops);
    const std::vector<git::RebaseOperation>& operations() const { return ops_; }
    void setOperationType(int row, git::RebaseOperationType type);

private:
    std::vector<git::RebaseOperation> ops_;
};

// ---------------------------------------------------------------------------
// RebaseOperationDelegate -- paints coloured operation label + hash + message
// ---------------------------------------------------------------------------
class RebaseOperationDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    explicit RebaseOperationDelegate(QObject* parent = nullptr);

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override;

private:
    static QColor colorForOperation(git::RebaseOperationType type);
    static QString labelForOperation(git::RebaseOperationType type);
};

// ---------------------------------------------------------------------------
// InteractiveRebaseWidget
// ---------------------------------------------------------------------------
class InteractiveRebaseWidget : public QWidget {
    Q_OBJECT
public:
    explicit InteractiveRebaseWidget(QWidget* parent = nullptr);

    /// Populate the widget with commits to rebase onto a target.
    void setCommits(const std::vector<git::CommitData>& commits,
                    const git::ObjectId& onto);

    /// Current rebase plan built from the UI state.
    git::RebasePlan rebasePlan() const;

    /// Clear all content.
    void clear();

signals:
    void rebaseRequested(const gitbolt::git::RebasePlan& plan);
    void rebaseAbortRequested();
    void rebaseContinueRequested();
    void rebaseSkipRequested();

private slots:
    void onStartRebase();

private:
    void setupUi();

    // Model / view
    RebaseListModel* model_ = nullptr;
    QListView* listView_ = nullptr;
    RebaseOperationDelegate* delegate_ = nullptr;

    // Toolbar buttons
    QToolBar* toolbar_ = nullptr;
    QPushButton* startBtn_ = nullptr;
    QPushButton* abortBtn_ = nullptr;
    QPushButton* continueBtn_ = nullptr;
    QPushButton* skipBtn_ = nullptr;

    // Onto target
    git::ObjectId onto_;
};

} // namespace gitbolt::widgets
