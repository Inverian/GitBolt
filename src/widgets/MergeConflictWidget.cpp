#include "widgets/MergeConflictWidget.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QSplitter>
#include <QVBoxLayout>

#include <utility>

namespace gitbolt::widgets {

static const QColor kOursColor(173, 216, 230, 60);    // light blue
static const QColor kBaseColor(211, 211, 211, 60);    // light gray
static const QColor kTheirsColor(144, 238, 144, 60);  // light green

MergeConflictWidget::MergeConflictWidget(QWidget* parent)
    : QWidget(parent)
{
    setupUI();
}

void MergeConflictWidget::setupUI() {
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(4, 4, 4, 4);

    // Navigation bar
    auto* navLayout = new QHBoxLayout;
    prevBtn_ = new QPushButton(tr("<< Prev"), this);
    nextBtn_ = new QPushButton(tr("Next >>"), this);
    auto* navLabel = new QLabel(tr("Conflict Navigation:"), this);
    navLayout->addWidget(navLabel);
    navLayout->addWidget(prevBtn_);
    navLayout->addWidget(nextBtn_);
    navLayout->addStretch();
    mainLayout->addLayout(navLayout);

    // Three-way panels: Ours | Base | Theirs
    auto* topSplitter = new QSplitter(Qt::Horizontal, this);

    // Ours panel
    auto* oursContainer = new QWidget(this);
    auto* oursLayout = new QVBoxLayout(oursContainer);
    oursLayout->setContentsMargins(0, 0, 0, 0);
    auto* oursLabel = new QLabel(tr("Ours (Local)"), this);
    oursLabel->setStyleSheet(QStringLiteral("font-weight: bold; color: #4169E1;"));
    oursLayout->addWidget(oursLabel);
    oursEditor_ = new QPlainTextEdit(this);
    oursEditor_->setReadOnly(true);
    oursEditor_->setLineWrapMode(QPlainTextEdit::NoWrap);
    oursLayout->addWidget(oursEditor_);
    topSplitter->addWidget(oursContainer);

    // Base panel
    auto* baseContainer = new QWidget(this);
    auto* baseLayout = new QVBoxLayout(baseContainer);
    baseLayout->setContentsMargins(0, 0, 0, 0);
    auto* baseLabel = new QLabel(tr("Base (Common Ancestor)"), this);
    baseLabel->setStyleSheet(QStringLiteral("font-weight: bold; color: #808080;"));
    baseLayout->addWidget(baseLabel);
    baseEditor_ = new QPlainTextEdit(this);
    baseEditor_->setReadOnly(true);
    baseEditor_->setLineWrapMode(QPlainTextEdit::NoWrap);
    baseLayout->addWidget(baseEditor_);
    topSplitter->addWidget(baseContainer);

    // Theirs panel
    auto* theirsContainer = new QWidget(this);
    auto* theirsLayout = new QVBoxLayout(theirsContainer);
    theirsLayout->setContentsMargins(0, 0, 0, 0);
    auto* theirsLabel = new QLabel(tr("Theirs (Remote)"), this);
    theirsLabel->setStyleSheet(QStringLiteral("font-weight: bold; color: #228B22;"));
    theirsLayout->addWidget(theirsLabel);
    theirsEditor_ = new QPlainTextEdit(this);
    theirsEditor_->setReadOnly(true);
    theirsEditor_->setLineWrapMode(QPlainTextEdit::NoWrap);
    theirsLayout->addWidget(theirsEditor_);
    topSplitter->addWidget(theirsContainer);

    // Main vertical splitter: top panels / result
    auto* mainSplitter = new QSplitter(Qt::Vertical, this);
    mainSplitter->addWidget(topSplitter);

    // Result panel (editable)
    auto* resultContainer = new QWidget(this);
    auto* resultLayout = new QVBoxLayout(resultContainer);
    resultLayout->setContentsMargins(0, 0, 0, 0);
    auto* resultLabel = new QLabel(tr("Merged Result (editable)"), this);
    resultLabel->setStyleSheet(QStringLiteral("font-weight: bold; color: #8B4513;"));
    resultLayout->addWidget(resultLabel);
    resultEditor_ = new QPlainTextEdit(this);
    resultEditor_->setObjectName(QStringLiteral("conflict.result"));
    resultEditor_->setLineWrapMode(QPlainTextEdit::NoWrap);
    resultLayout->addWidget(resultEditor_);
    mainSplitter->addWidget(resultContainer);

    mainSplitter->setStretchFactor(0, 2);
    mainSplitter->setStretchFactor(1, 1);
    mainLayout->addWidget(mainSplitter, 1);

    // Action buttons
    auto* btnLayout = new QHBoxLayout;

    acceptOursBtn_ = new QPushButton(tr("Accept Ours"), this);
    acceptOursBtn_->setIcon(QIcon::fromTheme(QStringLiteral("go-previous")));
    btnLayout->addWidget(acceptOursBtn_);

    acceptTheirsBtn_ = new QPushButton(tr("Accept Theirs"), this);
    acceptTheirsBtn_->setIcon(QIcon::fromTheme(QStringLiteral("go-next")));
    btnLayout->addWidget(acceptTheirsBtn_);

    acceptBothBtn_ = new QPushButton(tr("Accept Both"), this);
    btnLayout->addWidget(acceptBothBtn_);

    btnLayout->addStretch();

    markAllResolvedBtn_ = new QPushButton(tr("Mark All Resolved"), this);
    markAllResolvedBtn_->setStyleSheet(
        QStringLiteral("QPushButton { background-color: #228B22; color: white; "
                       "padding: 6px 12px; border-radius: 3px; }"));
    btnLayout->addWidget(markAllResolvedBtn_);

    abortMergeBtn_ = new QPushButton(tr("Abort Merge"), this);
    abortMergeBtn_->setStyleSheet(
        QStringLiteral("QPushButton { background-color: #B22222; color: white; "
                       "padding: 6px 12px; border-radius: 3px; }"));
    btnLayout->addWidget(abortMergeBtn_);

    mainLayout->addLayout(btnLayout);

    // Connections
    connect(acceptOursBtn_, &QPushButton::clicked, this, &MergeConflictWidget::onAcceptOurs);
    connect(acceptTheirsBtn_, &QPushButton::clicked, this, &MergeConflictWidget::onAcceptTheirs);
    connect(acceptBothBtn_, &QPushButton::clicked, this, &MergeConflictWidget::onAcceptBoth);
    connect(markAllResolvedBtn_, &QPushButton::clicked, this, &MergeConflictWidget::onMarkAllResolved);
    connect(abortMergeBtn_, &QPushButton::clicked, this, &MergeConflictWidget::onAbortMerge);
    connect(prevBtn_, &QPushButton::clicked, this, &MergeConflictWidget::onPrevConflict);
    connect(nextBtn_, &QPushButton::clicked, this, &MergeConflictWidget::onNextConflict);

    // Initial state: disabled
    acceptOursBtn_->setEnabled(false);
    acceptTheirsBtn_->setEnabled(false);
    acceptBothBtn_->setEnabled(false);
    markAllResolvedBtn_->setEnabled(false);
    prevBtn_->setEnabled(false);
    nextBtn_->setEnabled(false);
}

void MergeConflictWidget::setConflicts(
    const std::vector<git::MergeConflictEntry>& conflicts)
{
    conflicts_ = conflicts;
    resolvedContents_.clear();
    resolvedContents_.resize(conflicts_.size());

    if (!conflicts_.empty()) {
        acceptOursBtn_->setEnabled(true);
        acceptTheirsBtn_->setEnabled(true);
        acceptBothBtn_->setEnabled(true);
        markAllResolvedBtn_->setEnabled(true);
        showConflict(0);
    }
    updateNavigationState();
}

void MergeConflictWidget::clear() {
    conflicts_.clear();
    resolvedContents_.clear();
    currentConflict_ = -1;

    oursEditor_->clear();
    baseEditor_->clear();
    theirsEditor_->clear();
    resultEditor_->clear();

    acceptOursBtn_->setEnabled(false);
    acceptTheirsBtn_->setEnabled(false);
    acceptBothBtn_->setEnabled(false);
    markAllResolvedBtn_->setEnabled(false);
    prevBtn_->setEnabled(false);
    nextBtn_->setEnabled(false);
}

QString MergeConflictWidget::resolvedContent() const {
    if (currentConflict_ >= 0
        && currentConflict_ < static_cast<int>(resolvedContents_.size()))
        return resultEditor_->toPlainText();
    return {};
}

void MergeConflictWidget::showConflict(int index) {
    if (index < 0 || index >= static_cast<int>(conflicts_.size()))
        return;

    // Save current result before switching
    if (currentConflict_ >= 0
        && currentConflict_ < static_cast<int>(resolvedContents_.size()))
        resolvedContents_[static_cast<size_t>(currentConflict_)] =
            resultEditor_->toPlainText();

    currentConflict_ = index;
    const auto idx = static_cast<size_t>(index);  // index >= 0, checked above
    const auto& conflict = conflicts_[idx];

    oursEditor_->setPlainText(QString::fromStdString(conflict.oursContent));
    baseEditor_->setPlainText(QString::fromStdString(conflict.ancestorContent));
    theirsEditor_->setPlainText(QString::fromStdString(conflict.theirsContent));

    // If we already have a resolved version, show it; otherwise default to ours
    if (!resolvedContents_[idx].isEmpty()) {
        resultEditor_->setPlainText(resolvedContents_[idx]);
    } else {
        resultEditor_->setPlainText(QString::fromStdString(conflict.oursContent));
    }

    // Apply background highlights
    applyHighlighting(oursEditor_, kOursColor);
    applyHighlighting(baseEditor_, kBaseColor);
    applyHighlighting(theirsEditor_, kTheirsColor);

    updateNavigationState();

    // Update window title / label
    setWindowTitle(tr("Merge Conflict: %1 (%2/%3)")
                       .arg(QString::fromStdString(conflict.path))
                       .arg(index + 1)
                       .arg(static_cast<int>(conflicts_.size())));
}

void MergeConflictWidget::applyHighlighting(QPlainTextEdit* editor, const QColor& color) {
    // Highlight entire document background
    QTextCharFormat fmt;
    fmt.setBackground(color);

    QTextCursor cursor = editor->textCursor();
    cursor.movePosition(QTextCursor::Start);
    cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
    cursor.setCharFormat(fmt);
    cursor.clearSelection();
    editor->setTextCursor(cursor);
}

void MergeConflictWidget::updateNavigationState() {
    prevBtn_->setEnabled(currentConflict_ > 0);
    nextBtn_->setEnabled(
        currentConflict_ < static_cast<int>(conflicts_.size()) - 1);
}

void MergeConflictWidget::onAcceptOurs() {
    if (currentConflict_ < 0)
        return;
    const auto& conflict = conflicts_[static_cast<size_t>(currentConflict_)];
    resultEditor_->setPlainText(QString::fromStdString(conflict.oursContent));
}

void MergeConflictWidget::onAcceptTheirs() {
    if (currentConflict_ < 0)
        return;
    const auto& conflict = conflicts_[static_cast<size_t>(currentConflict_)];
    resultEditor_->setPlainText(QString::fromStdString(conflict.theirsContent));
}

void MergeConflictWidget::onAcceptBoth() {
    if (currentConflict_ < 0)
        return;
    const auto& conflict = conflicts_[static_cast<size_t>(currentConflict_)];
    QString combined = QString::fromStdString(conflict.oursContent);
    if (!combined.endsWith(QLatin1Char('\n')) && !combined.isEmpty())
        combined += QLatin1Char('\n');
    combined += QString::fromStdString(conflict.theirsContent);
    resultEditor_->setPlainText(combined);
}

void MergeConflictWidget::onMarkAllResolved() {
    // Save current
    if (currentConflict_ >= 0
        && currentConflict_ < static_cast<int>(resolvedContents_.size()))
        resolvedContents_[static_cast<size_t>(currentConflict_)] =
            resultEditor_->toPlainText();

    // Check that all conflicts have been visited
    bool allResolved = true;
    for (size_t i = 0; i < resolvedContents_.size(); ++i) {
        if (resolvedContents_[i].isEmpty()
            && std::cmp_not_equal(i, currentConflict_)) {
            allResolved = false;
            break;
        }
    }

    if (!allResolved) {
        auto answer = QMessageBox::question(
            this, tr("Unresolved Conflicts"),
            tr("Some conflicts have not been explicitly resolved.\n"
               "The current content in each result panel will be used.\n"
               "Continue marking all as resolved?"),
            QMessageBox::Yes | QMessageBox::No);
        if (answer != QMessageBox::Yes)
            return;

        // Fill in any unresolved ones with ours content as default
        for (size_t i = 0; i < resolvedContents_.size(); ++i) {
            if (resolvedContents_[i].isEmpty()) {
                resolvedContents_[i] =
                    QString::fromStdString(conflicts_[i].oursContent);
            }
        }
    }

    emit allConflictsResolved();
    emit conflictResolved();
}

void MergeConflictWidget::onAbortMerge() {
    auto answer = QMessageBox::question(
        this, tr("Abort Merge"),
        tr("Are you sure you want to abort the merge?\n"
           "All conflict resolutions will be lost."),
        QMessageBox::Yes | QMessageBox::No);
    if (answer == QMessageBox::Yes)
        emit mergeAborted();
}

void MergeConflictWidget::onPrevConflict() {
    if (currentConflict_ > 0)
        showConflict(currentConflict_ - 1);
}

void MergeConflictWidget::onNextConflict() {
    if (currentConflict_ < static_cast<int>(conflicts_.size()) - 1)
        showConflict(currentConflict_ + 1);
}

} // namespace gitbolt::widgets
