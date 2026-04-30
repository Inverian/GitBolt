#pragma once
#include "models/CommitLogModel.h"
#include <QWidget>

class QTableView;

namespace gitbolt::widgets {

class CommitFilterProxy;


class RevisionGraphWidget : public QWidget {
    Q_OBJECT
public:
    explicit RevisionGraphWidget(QWidget* parent = nullptr);
    void setModel(models::CommitLogModel* model);
    models::CommitLogModel* model() const;

    /// Find the row whose commit has the given full hex hash and
    /// select it (also scrolls to make it visible). Returns true on
    /// success; false if the model isn't attached or no row matches.
    /// Linear scan over the loaded commit window — fine for the
    /// ~256-row default page size; if paging grows we'll want a
    /// hash → row index on CommitLogModel instead.
    bool selectCommit(const QString& commitHash);

    /// Full hex SHA of the currently-selected commit, or an empty
    /// string if nothing is selected. Used by parent/child navigation
    /// and the back/forward history stack to capture the current
    /// position before moving.
    QString currentCommitHash() const;

    /// Move the selection to the current commit's first parent.
    /// Returns false if there's no current selection, the commit has
    /// no parents (root commit), or the parent isn't in the loaded
    /// commit window. We always pick `parentIds[0]` — for merge
    /// commits this is the "mainline" parent (where the user was
    /// before they merged), which is the conventional walk direction.
    bool selectFirstParent();

    /// Move the selection to the current commit's LAST parent —
    /// for a merge commit, the branch that was merged in (not the
    /// mainline). For a regular commit this is the same as
    /// selectFirstParent. Useful when the user wants to follow
    /// the merged-in branch instead of the integration branch.
    bool selectLastParent();

    /// Move the selection to a child of the current commit. Children
    /// are found by scanning EARLIER rows (lower index = newer in our
    /// model) for one whose parentIds contains the current commit.
    /// Returns false if no child is in the loaded window. If multiple
    /// children exist (branch point), we pick the first one we hit
    /// during the upward scan.
    bool selectFirstChild();

    /// Show or hide a single column in the revision graph table.
    /// Used by View menu toggles. Pass the column index from the
    /// CommitLogColumn enum (Graph / Message / Author / Date / Hash).
    /// No-ops if the index is out of range — guards against drift
    /// between this widget and the model's column enum.
    void setColumnVisible(int column, bool visible);

    /// Apply a substring filter to the commit message column.
    /// Empty string clears the filter. Uses case-insensitive
    /// matching. Implementation uses a CommitFilterProxy
    /// inserted between the source CommitLogModel and the table
    /// view so the filter is purely cosmetic; the source model's
    /// rowCount / commitAt remain intact.
    void setFilterText(const QString& text);

    /// Access the filter proxy for advanced multi-criterion
    /// filtering (author / sha / date range). Lifetime is tied
    /// to the widget. Never null after construction.
    CommitFilterProxy* filterProxy() const { return proxy_; }

    /// Move the selection to the next or previous visible row in
    /// the filtered view. Used by Quick search next/prev (Alt+Down
    /// / Alt+Up). Returns false if there's no selection or the
    /// edge of the filtered set is reached.
    bool selectNextMatch();
    bool selectPreviousMatch();

signals:
    /// Emitted whenever the current selection changes. The hash is
    /// the full hex SHA of the newly-selected commit, or an empty
    /// string when the selection is cleared (e.g. via modifier-
    /// click on the selected row). RepositoryView reacts to the
    /// empty-string case by blanking the inspector tabs.
    void commitSelected(const QString& commitHash);

protected:
    /// Implements modifier-click-to-deselect on the table viewport.
    /// QAbstractItemView in SingleSelection mode would otherwise
    /// keep exactly one row selected forever once anything is picked,
    /// so users couldn't get back to a "nothing selected" state.
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    void onSelectionChanged();
    void resizeGraphColumn();
    /// Size Author/Date/Hash by content then add a padding margin so text
    /// isn't cramped against the next column. ResizeToContents alone packs
    /// columns tight with zero breathing room; we switch those columns to
    /// Interactive and recompute their widths ourselves whenever the row
    /// set changes.
    void resizeMetaColumns();

private:
    /// Map the table view's current proxy index to the source
    /// model row, or -1 if there's no selection. Used by every
    /// nav helper that needs to look up a commit by row.
    int currentSourceRow() const;

    QTableView* tableView_ = nullptr;
    models::CommitLogModel* model_ = nullptr;
    CommitFilterProxy* proxy_ = nullptr;
};

} // namespace gitbolt::widgets
