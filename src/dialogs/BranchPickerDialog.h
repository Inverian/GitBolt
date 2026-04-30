#pragma once

#include <QDialog>
#include <QStringList>

class QLineEdit;
class QListWidget;
class QListWidgetItem;

namespace gitbolt::dialogs {

/// Lets the user pick which local branches feed the revision-graph
/// log walk. The host (MainWindow) calls setBranches() with the full
/// list and the previously-selected subset, shows the dialog
/// modally, and on Accepted reads selectedBranches() to drive
/// GitService::setSelectedBranches() + LogScope::SelectedBranches.
///
/// Each branch is shown as a checkable row. A search box at the top
/// narrows the visible list (substring, case-insensitive) so repos
/// with many branches stay manageable. Select All / Clear All
/// operate on the currently-visible filtered subset, not the whole
/// list — selecting all when only "feature/*" is visible only flips
/// those rows. This matches how every list-with-search dialog in
/// the rest of the app behaves and avoids surprise.
class BranchPickerDialog : public QDialog {
    Q_OBJECT
public:
    explicit BranchPickerDialog(QWidget* parent = nullptr);

    /// Populate the list. `allBranches` is every local branch (short
    /// names, no "refs/heads/" prefix); `selected` is the subset
    /// previously chosen, used to pre-check the rows.
    void setBranches(const QStringList& allBranches,
                     const QStringList& selected);

    /// The names whose checkbox is currently ticked. Order is
    /// alphabetical to match the list order.
    QStringList selectedBranches() const;

private:
    void setupUi();
    void rebuildList();
    void applyFilter(const QString& needle);

    QLineEdit*   search_   = nullptr;
    QListWidget* list_     = nullptr;
    QStringList  branches_;     // master list, alphabetical
    QStringList  selected_;     // ticked subset
};

} // namespace gitbolt::dialogs
