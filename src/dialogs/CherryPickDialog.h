#pragma once

#include "git/Commit.h"
#include "git/ObjectId.h"

#include <QDialog>
#include <vector>

class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QPushButton;

namespace gitbolt::dialogs {

class CherryPickDialog : public QDialog {
    Q_OBJECT
public:
    explicit CherryPickDialog(QWidget* parent = nullptr);

    /// Return the commit hash entered by the user.
    QString commitHash() const;

    /// Show details for a resolved commit (called by the host after lookup).
    void setCommitDetails(const gitbolt::git::CommitData& commit);

    /// Clear the commit details display (e.g. when input is invalid).
    void clearCommitDetails();

signals:
    /// Emitted when the input text changes so the host can validate / lookup.
    void commitHashChanged(const QString& hash);

    /// Emitted when the user clicks the browse button to open a commit log.
    void browseCommitsRequested();

private slots:
    void onHashEdited();

private:
    void setupUi();

    QLineEdit*        hashEdit_      = nullptr;
    QPushButton*      browseBtn_     = nullptr;
    QDialogButtonBox* buttonBox_     = nullptr;

    // Details area
    QLabel* summaryLabel_  = nullptr;
    QLabel* authorLabel_   = nullptr;
    QLabel* dateLabel_     = nullptr;
    QLabel* hashLabel_     = nullptr;
};

} // namespace gitbolt::dialogs
