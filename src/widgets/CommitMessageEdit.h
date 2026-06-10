#pragma once

#include <QPlainTextEdit>

namespace gitbolt::widgets {

/// A QPlainTextEdit for composing commit messages that paints a
/// dashed vertical guide at the 72-character column — the
/// conventional wrap point for commit body text. Used by the
/// commit dialog's message editor.
///
/// (Extracted from the retired CommitEditorWidget, whose
/// buttons/checkbox chrome was superseded by CommitDialog's own
/// commit-controls panel.)
class CommitMessageEdit : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit CommitMessageEdit(QWidget* parent = nullptr);

protected:
    void paintEvent(QPaintEvent* event) override;
};

} // namespace gitbolt::widgets
