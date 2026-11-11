#pragma once

#include "git/Commit.h"
#include "git/ObjectId.h"

#include <QDialog>
#include <QStringList>
#include <optional>
#include <vector>

class QComboBox;
class QDialogButtonBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QPushButton;

namespace gitbolt::dialogs {

/// Commands › Cherry pick and Revert commit: a commit to pick or revert,
/// with a preview of it. A merge's pick or revert asks which parent.
class CherryPickDialog : public QDialog {
    Q_OBJECT
public:
    enum class Mode { CherryPick, Revert };

    explicit CherryPickDialog(QWidget* parent = nullptr, Mode mode = Mode::CherryPick);

    Mode mode() const { return mode_; }

    /// Return the commit hash entered by the user.
    QString commitHash() const;

    /// Fill the commit field (the preview follows, through commitHashChanged).
    void setCommitHash(const QString& hash);

    /// Show details for a resolved commit (called by the host after lookup).
    /// `parentSummaries`: one line per parent, for a merge's parent chooser.
    void setCommitDetails(const gitbolt::git::CommitData& commit,
                          const QStringList& parentSummaries = {});

    /// Clear the commit details display (e.g. when input is invalid).
    void clearCommitDetails();

    /// The commit the preview shows, which OK picks or reverts: the
    /// field's text may name another by then (a branch that moved).
    std::optional<gitbolt::git::ObjectId> previewedCommit() const { return previewed_; }

    /// The parent (from 1) a merge is picked or reverted as seen from;
    /// 0 for any other commit.
    unsigned mainline() const;

signals:
    /// Emitted when the input text changes so the host can validate / lookup.
    void commitHashChanged(const QString& hash);

private slots:
    void onHashEdited();

private:
    void setupUi();

    Mode              mode_;
    QLineEdit*        hashEdit_      = nullptr;
    QDialogButtonBox* buttonBox_     = nullptr;

    // Details area
    QLabel* summaryLabel_  = nullptr;
    QLabel* authorLabel_   = nullptr;
    QLabel* dateLabel_     = nullptr;
    QLabel* hashLabel_     = nullptr;
    // A merge's parents, a row shown only for a merge.
    QFormLayout* detailsForm_ = nullptr;
    QComboBox*   parentCombo_ = nullptr;
    bool         merge_       = false;
    std::optional<gitbolt::git::ObjectId> previewed_;
};

} // namespace gitbolt::dialogs
