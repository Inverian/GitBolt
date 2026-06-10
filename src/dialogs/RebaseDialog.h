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

    /// Populate the commit list preview once a target is resolved.
    void setCommitsToRebase(const std::vector<gitbolt::git::CommitData>& commits,
                            const gitbolt::git::ObjectId& onto);

    /// Return the rebase plan assembled in the dialog.
    gitbolt::git::RebasePlan rebasePlan() const;

signals:
    /// Emitted when user selects a new target ref so the caller
    /// can resolve commits that would be rebased.
    void targetRefChanged(const QString& ref);

    /// Emitted when the user confirms the dialog (OK pressed) or
    /// clicks the embedded widget's Start Rebase button — both
    /// start paths funnel through this one signal.
    void rebaseRequested(const gitbolt::git::RebasePlan& plan);

    /// Forwards of the embedded InteractiveRebaseWidget's control
    /// buttons so the host can drive GitService::rebaseContinue /
    /// rebaseSkip / rebaseAbort when a started rebase pauses on a
    /// conflict. The widget enables these buttons after Start; the
    /// dialog stays open (shown non-modally) for exactly this.
    void rebaseContinueRequested();
    void rebaseSkipRequested();
    void rebaseAbortRequested();

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
};

} // namespace gitbolt::dialogs
