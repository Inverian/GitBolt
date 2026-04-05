#pragma once

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QPushButton>

namespace gitbolt::dialogs {

class WorktreeDialog : public QDialog {
    Q_OBJECT
public:
    explicit WorktreeDialog(QWidget* parent = nullptr);

    /// Populate the branch combo box with available branch names.
    void setBranches(const QStringList& branches);

    QString worktreeName() const;
    QString worktreePath() const;
    QString branch() const;
    bool createNewBranch() const;

private slots:
    void onBrowseClicked();
    void onCreateNewBranchToggled(bool checked);
    void validateInput();

private:
    void setupUI();

    QLineEdit* nameEdit_ = nullptr;
    QLineEdit* pathEdit_ = nullptr;
    QPushButton* browseBtn_ = nullptr;
    QComboBox* branchCombo_ = nullptr;
    QCheckBox* createBranchCheckBox_ = nullptr;
    QDialogButtonBox* buttonBox_ = nullptr;
};

} // namespace gitbolt::dialogs
