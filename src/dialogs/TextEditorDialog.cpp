#include "dialogs/TextEditorDialog.h"

#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QLabel>
#include <QPlainTextEdit>
#include <QVBoxLayout>

namespace gitbolt::dialogs {

TextEditorDialog::TextEditorDialog(QWidget* parent)
    : QDialog(parent) {
    setupUi();
}

void TextEditorDialog::setTitle(const QString& title) {
    setWindowTitle(title);
}

void TextEditorDialog::setLabel(const QString& text) {
    pathLabel_->setText(text);
}

void TextEditorDialog::setContents(const QString& text) {
    editor_->setPlainText(text);
    // Reset the undo stack so the user's first Cmd+Z doesn't blow
    // away the file's original contents — we want Cmd+Z to undo
    // their edits only, not the initial load.
    editor_->document()->clearUndoRedoStacks();
}

QString TextEditorDialog::contents() const {
    return editor_->toPlainText();
}

void TextEditorDialog::setupUi() {
    resize(720, 480);

    auto* layout = new QVBoxLayout(this);

    pathLabel_ = new QLabel(this);
    pathLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(pathLabel_);

    editor_ = new QPlainTextEdit(this);
    // Monospace font so column-aligned content (.gitattributes
    // patterns, .mailmap entries) keeps its alignment.
    editor_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    editor_->setLineWrapMode(QPlainTextEdit::NoWrap);
    layout->addWidget(editor_, /*stretch=*/1);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted,
            this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected,
            this, &QDialog::reject);
    layout->addWidget(buttons);
}

} // namespace gitbolt::dialogs
