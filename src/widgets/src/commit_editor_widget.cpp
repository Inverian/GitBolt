#include "gitbolt/widgets/commit_editor_widget.h"
#include <QVBoxLayout>
#include <QPlainTextEdit>
#include <QPushButton>

namespace gitbolt::widgets {

CommitEditorWidget::CommitEditorWidget(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    auto* editor = new QPlainTextEdit(this);
    editor->setPlaceholderText("Enter commit message...");
    layout->addWidget(editor);
    auto* btn = new QPushButton(tr("Commit"), this);
    layout->addWidget(btn);
}
QString CommitEditorWidget::message() const { return {}; }
void CommitEditorWidget::setMessage(const QString&) {}
void CommitEditorWidget::clear() {}


} // namespace gitbolt::widgets
