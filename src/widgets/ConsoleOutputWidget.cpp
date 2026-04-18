#include "widgets/ConsoleOutputWidget.h"

#include <QDateTime>
#include <QPlainTextEdit>
#include <QVBoxLayout>

namespace gitbolt::widgets {

ConsoleOutputWidget::ConsoleOutputWidget(QWidget* parent)
    : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    textEdit_ = new QPlainTextEdit(this);
    textEdit_->setReadOnly(true);
    textEdit_->setFont(QFont(QStringLiteral("Menlo"), 11));
    textEdit_->setStyleSheet(QStringLiteral(
        "QPlainTextEdit { background:#1e1e1e; color:#d4d4d4; "
        "border:none; padding:6px; }"));
    textEdit_->setPlaceholderText(
        tr("No console output yet — git commands will appear here."));
    layout->addWidget(textEdit_);
}

// Format: "[HH:mm:ss] <text>\n"
// Output uses the default foreground color; errors use a red accent
// inserted via inline HTML so the same widget can render both.
void ConsoleOutputWidget::appendOutput(const QString& text)
{
    if (!textEdit_ || text.isEmpty())
        return;
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
    textEdit_->appendPlainText(QStringLiteral("[%1] %2").arg(stamp, text));
}

void ConsoleOutputWidget::appendError(const QString& text)
{
    if (!textEdit_ || text.isEmpty())
        return;
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
    // Use appendHtml so we can colorize the error line. Escape the
    // user-supplied text to keep stray HTML characters inert.
    const QString line = QStringLiteral(
        "<span style='color:#f48771;'>[%1] %2</span>")
        .arg(stamp, text.toHtmlEscaped());
    textEdit_->appendHtml(line);
}

void ConsoleOutputWidget::clear()
{
    if (textEdit_)
        textEdit_->clear();
}

} // namespace gitbolt::widgets
