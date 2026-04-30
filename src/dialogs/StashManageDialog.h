#pragma once

#include "git/Stash.h"

#include <QDialog>
#include <vector>

class QListWidget;
class QPushButton;

namespace gitbolt::dialogs {

/// "Manage stashes" dialog. Shows the existing stash entries for the
/// current repository in a list and exposes Apply / Pop / Drop on the
/// selected entry plus a "Save new stash..." button that round-trips
/// to StashDialog. The dialog itself is purely presentational —
/// actions are emitted as signals and the host (MainWindow) executes
/// them against GitService / Repository, then refreshes the list.
class StashManageDialog : public QDialog {
    Q_OBJECT
public:
    explicit StashManageDialog(QWidget* parent = nullptr);

    /// Replace the currently-displayed list of stashes. Called by
    /// the host both when the dialog opens and after each action so
    /// the user sees the result of their Apply / Pop / Drop.
    void setStashes(const std::vector<gitbolt::git::StashEntry>& stashes);

signals:
    /// Apply the stash at `index` to the working tree without
    /// removing it from the stash list (`git stash apply <i>`).
    void applyRequested(size_t index);

    /// Apply the stash at `index` and drop it on success
    /// (`git stash pop <i>`).
    void popRequested(size_t index);

    /// Drop the stash at `index` without applying it
    /// (`git stash drop <i>`).
    void dropRequested(size_t index);

    /// Open the existing StashDialog to capture a new stash. The
    /// host wires the result back through GitService::stashSave.
    void newStashRequested();

private:
    void setupUi();
    void onSelectionChanged();
    /// Resolve the currently-selected list-row to a stash index.
    /// Returns -1 if no row is selected. Called by every button
    /// click so the buttons can stay enabled-but-noop when nothing
    /// is selected (instead of having to re-toggle their enabled
    /// state on every selectionChanged signal).
    int selectedIndex() const;

    QListWidget* list_     = nullptr;
    QPushButton* applyBtn_ = nullptr;
    QPushButton* popBtn_   = nullptr;
    QPushButton* dropBtn_  = nullptr;
    QPushButton* newBtn_   = nullptr;
    QPushButton* closeBtn_ = nullptr;
};

} // namespace gitbolt::dialogs
