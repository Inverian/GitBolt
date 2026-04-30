#pragma once

#include <QDialog>

class QLabel;
class QPlainTextEdit;

namespace gitbolt::dialogs {

/// Minimal monospace text editor dialog used for the
/// "Edit .gitignore / .git/info/exclude / .gitattributes / .mailmap"
/// menu items. The dialog is purely a UI surface — file I/O lives in
/// the caller, which loads the initial text via setContents() and
/// writes the final text returned by contents() on Accept.
///
/// Kept deliberately simple: a single QPlainTextEdit, monospace font,
/// undo/redo via Qt's built-in stack, and a Save / Cancel button row.
/// No syntax highlighting — these are config files, not code, and
/// adding a highlighter for each file format isn't worth the cost.
class TextEditorDialog : public QDialog {
    Q_OBJECT
public:
    explicit TextEditorDialog(QWidget* parent = nullptr);

    /// Window title shown in the dialog's title bar.
    void setTitle(const QString& title);

    /// Descriptive label rendered above the editor (e.g. file path).
    void setLabel(const QString& text);

    /// Set the initial contents shown in the editor. Call before
    /// exec(). Subsequent calls reset the editor and clear the
    /// undo stack.
    void setContents(const QString& text);

    /// Return the current contents of the editor. Call after exec()
    /// returns Accepted.
    QString contents() const;

private:
    void setupUi();

    QLabel*         pathLabel_ = nullptr;
    QPlainTextEdit* editor_    = nullptr;
};

} // namespace gitbolt::dialogs
