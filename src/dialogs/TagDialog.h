#pragma once

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QRadioButton>

namespace gitbolt::dialogs {

class TagDialog : public QDialog {
    Q_OBJECT
public:
    explicit TagDialog(QWidget* parent = nullptr);

    /// Populate the target combo box with available branch names.
    void setBranches(const QStringList& branches);

    QString tagName() const;
    QString targetRef() const;
    QString message() const;
    bool isAnnotated() const;
    bool shouldPush() const;

private slots:
    void onTagTypeChanged();
    void validateInput();

private:
    void setupUI();

    QLineEdit* nameEdit_ = nullptr;
    QComboBox* targetCombo_ = nullptr;
    QLineEdit* targetRefEdit_ = nullptr;
    QRadioButton* annotatedRadio_ = nullptr;
    QRadioButton* lightweightRadio_ = nullptr;
    QPlainTextEdit* messageEdit_ = nullptr;
    QCheckBox* pushCheckBox_ = nullptr;
    QDialogButtonBox* buttonBox_ = nullptr;
};

} // namespace gitbolt::dialogs
