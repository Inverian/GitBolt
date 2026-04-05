#include "widgets/CherryPickWidget.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QToolBar>
#include <QVBoxLayout>

namespace gitbolt::widgets {

CherryPickWidget::CherryPickWidget(QWidget* parent)
    : QWidget(parent)
    , commitInput_(new QLineEdit(this))
    , addBtn_(new QPushButton(tr("Add Commit"), this))
    , commitList_(new QListWidget(this))
    , removeBtn_(new QPushButton(tr("Remove"), this))
    , noCommitCheck_(new QCheckBox(tr("No commit (apply changes without committing)"), this))
    , pickBtn_(new QPushButton(tr("Cherry-Pick"), this))
    , abortBtn_(new QPushButton(tr("Abort"), this))
    , statusLabel_(new QLabel(this))
    , toolbar_(new QToolBar(this))
{
    setupUi();
}

void CherryPickWidget::setupUi() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(6);

    // Input row: line-edit + add button
    auto* inputLayout = new QHBoxLayout;
    commitInput_->setPlaceholderText(tr("Enter commit hash or branch name"));
    inputLayout->addWidget(commitInput_, 1);
    inputLayout->addWidget(addBtn_);
    layout->addLayout(inputLayout);

    // Commit list
    auto* listLabel = new QLabel(tr("Commits to cherry-pick:"), this);
    layout->addWidget(listLabel);

    commitList_->setSelectionMode(QAbstractItemView::SingleSelection);
    commitList_->setAlternatingRowColors(true);
    layout->addWidget(commitList_, 1);

    // Remove button
    removeBtn_->setEnabled(false);
    layout->addWidget(removeBtn_);

    // Options
    layout->addWidget(noCommitCheck_);

    // Toolbar row
    toolbar_->setIconSize(QSize(16, 16));
    toolbar_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toolbar_->addWidget(pickBtn_);
    toolbar_->addSeparator();
    toolbar_->addWidget(abortBtn_);
    abortBtn_->setEnabled(false);
    layout->addWidget(toolbar_);

    // Status area
    statusLabel_->setWordWrap(true);
    statusLabel_->setStyleSheet(
        QStringLiteral("QLabel { padding: 4px; color: #666; }"));
    layout->addWidget(statusLabel_);

    // Connections
    connect(addBtn_, &QPushButton::clicked,
            this, &CherryPickWidget::onAddCommit);
    connect(removeBtn_, &QPushButton::clicked,
            this, &CherryPickWidget::onRemoveSelected);
    connect(pickBtn_, &QPushButton::clicked,
            this, &CherryPickWidget::onCherryPick);
    connect(abortBtn_, &QPushButton::clicked,
            this, &CherryPickWidget::onAbort);
    connect(commitInput_, &QLineEdit::returnPressed,
            this, &CherryPickWidget::onAddCommit);

    connect(commitList_, &QListWidget::itemSelectionChanged, this, [this]() {
        removeBtn_->setEnabled(!commitList_->selectedItems().isEmpty());
    });
}

bool CherryPickWidget::isValidHex(const QString& hex) const {
    static QRegularExpression hexPattern(
        QStringLiteral("^[0-9a-fA-F]{4,40}$"));
    return hexPattern.match(hex).hasMatch();
}

void CherryPickWidget::onAddCommit() {
    QString text = commitInput_->text().trimmed();
    if (text.isEmpty())
        return;

    if (!isValidHex(text)) {
        // Allow branch names or refs as well -- just add it
        // Validation against the actual repo is done by the service layer
    }

    // Prevent duplicates
    for (int i = 0; i < commitList_->count(); ++i) {
        if (commitList_->item(i)->text() == text)
            return;
    }

    commitList_->addItem(text);
    commitInput_->clear();
    commitInput_->setFocus();
}

void CherryPickWidget::onRemoveSelected() {
    auto items = commitList_->selectedItems();
    for (auto* item : items)
        delete commitList_->takeItem(commitList_->row(item));
}

void CherryPickWidget::onCherryPick() {
    auto commits = selectedCommits();
    if (commits.empty()) {
        setStatusText(tr("No commits selected for cherry-pick."));
        return;
    }

    pickBtn_->setEnabled(false);
    abortBtn_->setEnabled(true);
    setStatusText(tr("Cherry-pick in progress..."));

    emit cherryPickRequested(commits);
}

void CherryPickWidget::onAbort() {
    abortBtn_->setEnabled(false);
    pickBtn_->setEnabled(true);
    setStatusText(tr("Cherry-pick aborted."));
    emit cherryPickAborted();
}

std::vector<git::ObjectId> CherryPickWidget::selectedCommits() const {
    std::vector<git::ObjectId> result;
    result.reserve(commitList_->count());
    for (int i = 0; i < commitList_->count(); ++i) {
        QString hex = commitList_->item(i)->text().trimmed();
        if (isValidHex(hex))
            result.push_back(git::ObjectId::fromHex(hex.toStdString()));
    }
    return result;
}

bool CherryPickWidget::noCommitMode() const {
    return noCommitCheck_->isChecked();
}

void CherryPickWidget::setStatusText(const QString& text) {
    statusLabel_->setText(text);
}

void CherryPickWidget::clear() {
    commitInput_->clear();
    commitList_->clear();
    noCommitCheck_->setChecked(false);
    statusLabel_->clear();
    pickBtn_->setEnabled(true);
    abortBtn_->setEnabled(false);
    removeBtn_->setEnabled(false);
}

} // namespace gitbolt::widgets
