#pragma once

#include <QDialog>

class QCheckBox;
class QLineEdit;

namespace gitbolt::dialogs {

class StashDialog : public QDialog {
    Q_OBJECT
public:
    explicit StashDialog(QWidget* parent = nullptr);

    /// Stash message entered by the user (may be empty).
    QString message() const;

    /// Whether the "Include untracked files" checkbox is checked.
    bool includeUntracked() const;

    /// Whether the "Keep index" checkbox is checked.
    bool keepIndex() const;

private:
    void setupUi();

    QLineEdit* messageEdit_       = nullptr;
    QCheckBox* untrackedCheck_    = nullptr;
    QCheckBox* keepIndexCheck_    = nullptr;
};

} // namespace gitbolt::dialogs
