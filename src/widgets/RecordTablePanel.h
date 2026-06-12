#pragma once

#include <QWidget>

class QAbstractTableModel;
class QTableView;
class QToolBar;

namespace gitbolt::widgets {

/// Shared scaffold for the flat "toolbar above a record table"
/// inspector panels (worktrees, submodules — the next such panel
/// gets it for free): a vertical layout holding a small icon
/// toolbar and a single-row-select QTableView configured the one
/// way all these panels want it (row selection, no grid, hidden
/// vertical header, content-sized columns with a stretched tail).
///
/// Subclasses add their toolbar actions in their constructor,
/// attach their model via initPanel(), and use selectedRow()
/// instead of re-rolling the selectedRows() dance per accessor.
class RecordTablePanel : public QWidget {
    Q_OBJECT
protected:
    explicit RecordTablePanel(QWidget* parent = nullptr);

    /// Attach the panel's model and finish the view setup. Call
    /// once from the subclass constructor after the model exists.
    void initPanel(QAbstractTableModel* model);

    /// Currently selected row in model coordinates, or -1 when
    /// nothing is selected.
    int selectedRow() const;

    QTableView* table_ = nullptr;
    QToolBar*   toolbar_ = nullptr;
};

} // namespace gitbolt::widgets
