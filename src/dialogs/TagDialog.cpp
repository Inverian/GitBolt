#include "dialogs/TagDialog.h"
#include "conf/SettingsService.h"

#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace gitbolt::dialogs {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

TagDialog::TagDialog(QWidget* parent)
    : QDialog(parent)
{
    setupUI();
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

void TagDialog::setBranches(const QStringList& branches) {
    targetCombo_->clear();
    targetCombo_->addItems(branches);
}

QString TagDialog::tagName() const {
    return nameEdit_->text().trimmed();
}

QString TagDialog::targetRef() const {
    // Prefer the explicit ref/hash line edit if it has content
    const QString custom = targetRefEdit_->text().trimmed();
    if (!custom.isEmpty())
        return custom;
    return targetCombo_->currentText();
}

QString TagDialog::message() const {
    return messageEdit_->toPlainText().trimmed();
}

bool TagDialog::isAnnotated() const {
    return annotatedRadio_->isChecked();
}

bool TagDialog::shouldPush() const {
    return pushCheckBox_->isChecked();
}

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------

void TagDialog::onTagTypeChanged() {
    const bool annotated = annotatedRadio_->isChecked();
    messageEdit_->setEnabled(annotated);
    if (!annotated)
        messageEdit_->setPlaceholderText(tr("(Lightweight tags have no message)"));
    else
        messageEdit_->setPlaceholderText(tr("Enter tag message..."));
}

void TagDialog::validateInput() {
    const bool valid = !nameEdit_->text().trimmed().isEmpty();
    buttonBox_->button(QDialogButtonBox::Ok)->setEnabled(valid);
}

// ---------------------------------------------------------------------------
// UI setup
// ---------------------------------------------------------------------------

void TagDialog::setupUI() {
    setWindowTitle(tr("Create Tag"));
    // Global dialog default + per-dialog restore — see
    // Settings → UI Design → Default Dialog Size. Helper sets
    // initial size and wires up save-on-close.
    conf::SettingsService::applyConfiguredSize(this, "tag");

    auto* mainLayout = new QVBoxLayout(this);

    // --- Form fields -------------------------------------------------------
    auto* formLayout = new QFormLayout;
    formLayout->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    // Tag name
    nameEdit_ = new QLineEdit(this);
    nameEdit_->setPlaceholderText(tr("v1.0.0"));
    formLayout->addRow(tr("Tag name:"), nameEdit_);

    // Target branch combo
    targetCombo_ = new QComboBox(this);
    targetCombo_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    formLayout->addRow(tr("Target branch:"), targetCombo_);

    // Arbitrary ref/hash
    targetRefEdit_ = new QLineEdit(this);
    targetRefEdit_->setPlaceholderText(tr("Or enter a ref / commit hash"));
    formLayout->addRow(tr("Or target ref:"), targetRefEdit_);

    mainLayout->addLayout(formLayout);

    // --- Tag type group -----------------------------------------------------
    auto* typeGroup = new QGroupBox(tr("Tag Type"), this);
    auto* typeLayout = new QHBoxLayout(typeGroup);

    annotatedRadio_ = new QRadioButton(tr("Annotated"), typeGroup);
    lightweightRadio_ = new QRadioButton(tr("Lightweight"), typeGroup);
    annotatedRadio_->setChecked(true);

    typeLayout->addWidget(annotatedRadio_);
    typeLayout->addWidget(lightweightRadio_);
    typeLayout->addStretch();

    mainLayout->addWidget(typeGroup);

    // --- Message -----------------------------------------------------------
    messageEdit_ = new QPlainTextEdit(this);
    messageEdit_->setPlaceholderText(tr("Enter tag message..."));
    messageEdit_->setMaximumHeight(100);
    mainLayout->addWidget(new QLabel(tr("Message:"), this));
    mainLayout->addWidget(messageEdit_);

    // --- Push checkbox -----------------------------------------------------
    pushCheckBox_ = new QCheckBox(tr("Push tag after creation"), this);
    mainLayout->addWidget(pushCheckBox_);

    // --- Button box --------------------------------------------------------
    buttonBox_ = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttonBox_->button(QDialogButtonBox::Ok)->setText(tr("Create"));
    buttonBox_->button(QDialogButtonBox::Ok)->setEnabled(false);
    mainLayout->addWidget(buttonBox_);

    // --- Connections -------------------------------------------------------
    connect(annotatedRadio_, &QRadioButton::toggled,
            this, &TagDialog::onTagTypeChanged);
    connect(lightweightRadio_, &QRadioButton::toggled,
            this, &TagDialog::onTagTypeChanged);
    connect(nameEdit_, &QLineEdit::textChanged,
            this, &TagDialog::validateInput);
    connect(buttonBox_, &QDialogButtonBox::accepted,
            this, &QDialog::accept);
    connect(buttonBox_, &QDialogButtonBox::rejected,
            this, &QDialog::reject);
}

} // namespace gitbolt::dialogs
