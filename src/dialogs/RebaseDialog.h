#pragma once

#include "git/Branch.h"
#include "git/Commit.h"
#include "git/Rebase.h"

#include <QDialog>
#include <vector>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;

namespace gitbolt::widgets {
class InteractiveRebaseWidget;
}

namespace gitbolt::dialogs {

class RebaseDialog : public QDialog {
    Q_OBJECT
public:
    explicit RebaseDialog(QWidget* parent = nullptr);

    /// Populate the branch selector with available branches.
    void setBranches(const std::vector<gitbolt::git::BranchInfo>& branches);

    /// Populate the commit list preview once a target is resolved:
    /// the commits from HEAD (at `head`, on `branch`; empty when
    /// detached) down to `onto`, newest first. Merges among them are
    /// listed, marked as not replayed, and left out of the plan.
    void setCommitsToRebase(const std::vector<gitbolt::git::CommitData>& commits,
                            const gitbolt::git::ObjectId& onto,
                            const gitbolt::git::ObjectId& head,
                            const std::string& branch);

    /// Return the rebase plan assembled in the dialog.
    gitbolt::git::RebasePlan rebasePlan() const;

    /// For a rebaseRequested handler that can't start the rebase: the
    /// dialog says why and stays open, the plan as it was.
    void refuse(const QString& reason);

signals:
    /// Emitted when user selects a new target ref so the caller
    /// can resolve commits that would be rebased.
    void targetRefChanged(const QString& ref);

    /// Emitted when the user confirms the dialog (its Rebase button),
    /// just before it closes — unless a handler calls refuse(). A
    /// rebase that then stops — a conflict, an `edit` — is carried on
    /// from the repository view's in-progress bar, not from here: this
    /// dialog used to hold the only Continue / Skip / Abort, and only
    /// while it stayed open.
    void rebaseRequested(const gitbolt::git::RebasePlan& plan);

private slots:
    void onBranchSelected(int index);
    void onRefEdited();
    void onAccepted();

private:
    void setupUi();

    QComboBox*  branchCombo_  = nullptr;
    QLineEdit*  refEdit_      = nullptr;
    QLabel*     previewLabel_ = nullptr;
    QListWidget* previewList_ = nullptr;

    widgets::InteractiveRebaseWidget* rebaseWidget_ = nullptr;

    // What refuse() was given while rebaseRequested was out.
    QString refusal_;
};

} // namespace gitbolt::dialogs
