#include "widgets/CommitEditorWidget.h"

#include <QCheckBox>
#include <QFontMetricsF>
#include <QHBoxLayout>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QShortcut>
#include <QVBoxLayout>

namespace gitbolt::widgets {

// ===========================================================================
// CommitMessageEdit
// ===========================================================================

CommitMessageEdit::CommitMessageEdit(QWidget* parent)
    : QPlainTextEdit(parent)
{
    setPlaceholderText(tr("Enter commit message..."));
    setTabChangesFocus(false);
    setLineWrapMode(QPlainTextEdit::NoWrap);

    QFont monoFont(QStringLiteral("Menlo, Consolas, monospace"));
    monoFont.setStyleHint(QFont::Monospace);
    monoFont.setPointSize(12);
    setFont(monoFont);
}

void CommitMessageEdit::paintEvent(QPaintEvent* event)
{
    // Draw the base widget first
    QPlainTextEdit::paintEvent(event);

    // Paint a vertical guide line at the 72-character column
    QPainter painter(viewport());
    const QFontMetricsF fm(font());
    const qreal charWidth = fm.averageCharWidth();
    const int guideX = static_cast<int>(charWidth * 72.0)
                       - horizontalScrollBar()->value()
                       + contentOffset().x();

    painter.setPen(QPen(QColor(200, 200, 200, 128), 1, Qt::DashLine));
    painter.drawLine(guideX, 0, guideX, viewport()->height());
}

// ===========================================================================
// CommitEditorWidget
// ===========================================================================

CommitEditorWidget::CommitEditorWidget(QWidget* parent)
    : QWidget(parent)
{
    setupUi();
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

QString CommitEditorWidget::message() const
{
    return editor_->toPlainText();
}

void CommitEditorWidget::setMessage(const QString& msg)
{
    editor_->setPlainText(msg);
    // Move cursor to end
    QTextCursor cursor = editor_->textCursor();
    cursor.movePosition(QTextCursor::End);
    editor_->setTextCursor(cursor);
}

void CommitEditorWidget::clear()
{
    editor_->clear();
    amendCheck_->setChecked(false);
}

bool CommitEditorWidget::isAmend() const
{
    return amendCheck_->isChecked();
}

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------

void CommitEditorWidget::onCommitClicked()
{
    const QString msg = message().trimmed();
    if (msg.isEmpty())
        return;
    emit commitRequested(msg, amendCheck_->isChecked());
}

void CommitEditorWidget::onCommitAndPushClicked()
{
    const QString msg = message().trimmed();
    if (msg.isEmpty())
        return;
    emit commitAndPushRequested(msg);
}

// ---------------------------------------------------------------------------
// UI setup
// ---------------------------------------------------------------------------

void CommitEditorWidget::setupUi()
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);

    // ---- Commit message editor --------------------------------------------
    editor_ = new CommitMessageEdit(this);
    editor_->setMinimumHeight(80);
    layout->addWidget(editor_, 1);

    // ---- Amend checkbox ---------------------------------------------------
    amendCheck_ = new QCheckBox(tr("Amend last commit"), this);
    layout->addWidget(amendCheck_);

    // ---- Buttons ----------------------------------------------------------
    auto* btnLayout = new QHBoxLayout;
    btnLayout->setContentsMargins(0, 0, 0, 0);

    commitBtn_ = new QPushButton(tr("Commit"), this);
    commitBtn_->setDefault(true);
    btnLayout->addWidget(commitBtn_);

    commitPushBtn_ = new QPushButton(tr("Commit && Push"), this);
    btnLayout->addWidget(commitPushBtn_);

    layout->addLayout(btnLayout);

    // ---- Connections ------------------------------------------------------
    connect(commitBtn_, &QPushButton::clicked,
            this, &CommitEditorWidget::onCommitClicked);
    connect(commitPushBtn_, &QPushButton::clicked,
            this, &CommitEditorWidget::onCommitAndPushClicked);

    // ---- Keyboard shortcut: Ctrl+Enter to commit --------------------------
    commitShortcut_ = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return), this);
    connect(commitShortcut_, &QShortcut::activated,
            this, &CommitEditorWidget::onCommitClicked);
}

} // namespace gitbolt::widgets
