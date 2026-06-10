#pragma once

#include "git/Reflog.h"

#include <QDialog>
#include <vector>

class QComboBox;
class QPushButton;
class QTableWidget;

namespace gitbolt::dialogs {

/// Modeless window showing the reflog for a chosen ref. The user
/// picks the ref from a combo (HEAD plus every local branch) and
/// the table refreshes with that ref's reflog entries — old SHA,
/// new SHA, committer, when, message — in chronological order
/// (oldest at the top).
///
/// Right-clicking a row offers recovery actions on that entry's
/// "new" SHA (the state the ref moved to): checkout (detached
/// HEAD) and reset of the current branch (soft / mixed / hard).
/// The dialog only EMITS for these — the host owns the git calls
/// and the confirmation prompts, mirroring the refSelected /
/// setEntries split used for data loading.
class ReflogDialog : public QDialog {
    Q_OBJECT
public:
    explicit ReflogDialog(QWidget* parent = nullptr);

    /// Populate the ref picker. Pass HEAD as the first entry plus
    /// every local branch's full ref name. The host computes the
    /// list because the dialog has no Repository handle.
    void setRefs(const QStringList& refs);

    /// Replace the table with reflog entries for the currently-
    /// selected ref. Called by the host after each selection
    /// change (the host listens to `refSelected` and does the
    /// `Repository::reflog(ref)` call there).
    void setEntries(const std::vector<gitbolt::git::ReflogEntry>& entries);

    /// Re-emit refSelected for the current combo entry so the
    /// host reloads the table — called by the host after a
    /// checkout/reset so the reflog shows the operation it just
    /// performed.
    void refreshCurrentRef();

signals:
    /// Emitted when the user picks a different ref. Host should
    /// resolve the reflog and call setEntries() with the result.
    void refSelected(const QString& ref);

    /// Context-menu actions on a reflog row. `sha` is the row's
    /// full "new" SHA; `mode` is "soft", "mixed", or "hard".
    void checkoutRequested(const QString& sha);
    void resetRequested(const QString& sha, const QString& mode);

private:
    void setupUi();
    void onTableContextMenu(const QPoint& pos);

    QComboBox*   refCombo_ = nullptr;
    QTableWidget* table_   = nullptr;
    QPushButton* closeBtn_ = nullptr;
};

} // namespace gitbolt::dialogs
