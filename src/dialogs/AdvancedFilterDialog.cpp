#include "dialogs/AdvancedFilterDialog.h"

#include <QCheckBox>
#include <QDateEdit>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace gitbolt::dialogs {

AdvancedFilterDialog::AdvancedFilterDialog(QWidget* parent)
    : QDialog(parent) {
    setupUi();
}

void AdvancedFilterDialog::setCriteria(
        const widgets::CommitFilterProxy::Criteria& c) {
    msgEnabled_->setChecked(!c.messageContains.isEmpty());
    msgEdit_->setText(c.messageContains);
    authorEnabled_->setChecked(!c.authorContains.isEmpty());
    authorEdit_->setText(c.authorContains);
    shaEnabled_->setChecked(!c.shaPrefix.isEmpty());
    shaEdit_->setText(c.shaPrefix);
    fromEnabled_->setChecked(c.fromDate.isValid());
    if (c.fromDate.isValid()) fromEdit_->setDate(c.fromDate);
    toEnabled_->setChecked(c.toDate.isValid());
    if (c.toDate.isValid()) toEdit_->setDate(c.toDate);
}

widgets::CommitFilterProxy::Criteria
AdvancedFilterDialog::criteria() const {
    widgets::CommitFilterProxy::Criteria c;
    if (msgEnabled_->isChecked())
        c.messageContains = msgEdit_->text().trimmed();
    if (authorEnabled_->isChecked())
        c.authorContains = authorEdit_->text().trimmed();
    if (shaEnabled_->isChecked())
        c.shaPrefix = shaEdit_->text().trimmed();
    if (fromEnabled_->isChecked()) c.fromDate = fromEdit_->date();
    if (toEnabled_->isChecked())   c.toDate   = toEdit_->date();
    return c;
}

void AdvancedFilterDialog::setupUi() {
    setWindowTitle(tr("Advanced Filter"));
    setMinimumWidth(520);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(20, 20, 20, 16);
    root->setSpacing(14);

    auto* intro = new QLabel(tr(
        "Filter the revision grid by any combination of the "
        "criteria below. Tick the checkbox next to a row to "
        "make that criterion active. Empty unticked rows are "
        "ignored."), this);
    intro->setWordWrap(true);
    root->addWidget(intro);

    // Form: each row pairs a checkbox (which makes the criterion
    // active) with the input field. Disable the input when the
    // box is unticked so users can't fight the criteria-isAny-
    // Active gate accidentally.
    auto* form = new QFormLayout();
    form->setLabelAlignment(Qt::AlignLeft);
    form->setHorizontalSpacing(12);
    form->setVerticalSpacing(10);

    auto wireRow = [](QCheckBox* cb, QWidget* field) {
        QObject::connect(cb, &QCheckBox::toggled,
                         field, &QWidget::setEnabled);
        field->setEnabled(cb->isChecked());
    };

    msgEnabled_ = new QCheckBox(tr("Message contains"), this);
    msgEdit_ = new QLineEdit(this);
    msgEdit_->setPlaceholderText(tr("substring, case-insensitive"));
    form->addRow(msgEnabled_, msgEdit_);
    wireRow(msgEnabled_, msgEdit_);

    authorEnabled_ = new QCheckBox(tr("Author contains"), this);
    authorEdit_ = new QLineEdit(this);
    authorEdit_->setPlaceholderText(tr("name or email substring"));
    form->addRow(authorEnabled_, authorEdit_);
    wireRow(authorEnabled_, authorEdit_);

    shaEnabled_ = new QCheckBox(tr("SHA starts with"), this);
    shaEdit_ = new QLineEdit(this);
    shaEdit_->setPlaceholderText(tr("hex prefix, any length"));
    form->addRow(shaEnabled_, shaEdit_);
    wireRow(shaEnabled_, shaEdit_);

    fromEnabled_ = new QCheckBox(tr("From date"), this);
    fromEdit_ = new QDateEdit(QDate::currentDate().addMonths(-1), this);
    fromEdit_->setDisplayFormat("yyyy-MM-dd");
    fromEdit_->setCalendarPopup(true);
    form->addRow(fromEnabled_, fromEdit_);
    wireRow(fromEnabled_, fromEdit_);

    toEnabled_ = new QCheckBox(tr("To date"), this);
    toEdit_ = new QDateEdit(QDate::currentDate(), this);
    toEdit_->setDisplayFormat("yyyy-MM-dd");
    toEdit_->setCalendarPopup(true);
    form->addRow(toEnabled_, toEdit_);
    wireRow(toEnabled_, toEdit_);

    root->addLayout(form);
    root->addStretch(1);

    // Three actions: Apply (returns Accepted with current
    // criteria), Clear All (un-ticks every checkbox so the
    // returned criteria are inactive), Cancel (no change).
    auto* btnRow = new QHBoxLayout();
    auto* clearBtn = new QPushButton(tr("Clear All"), this);
    btnRow->addWidget(clearBtn);
    btnRow->addStretch(1);
    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Apply | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Apply)->setDefault(true);
    btnRow->addWidget(buttons);
    root->addLayout(btnRow);

    connect(buttons->button(QDialogButtonBox::Apply),
            &QPushButton::clicked, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected,
            this, &QDialog::reject);
    connect(clearBtn, &QPushButton::clicked, this, [this]() {
        msgEnabled_->setChecked(false);
        authorEnabled_->setChecked(false);
        shaEnabled_->setChecked(false);
        fromEnabled_->setChecked(false);
        toEnabled_->setChecked(false);
        msgEdit_->clear();
        authorEdit_->clear();
        shaEdit_->clear();
    });
}

} // namespace gitbolt::dialogs
