#pragma once

#include <QPlainTextEdit>

namespace gitbolt::widgets {

/// QPlainTextEdit specialised for diff rendering.
///
/// Overrides paintEvent so addition / deletion / hunk-header lines
/// receive a *full-line-height* coloured background that abuts its
/// neighbour — no white sliver in the inter-block leading. The base
/// QPlainTextEdit only paints under the text itself (via
/// QTextCharFormat::setBackground or QTextEdit::ExtraSelection +
/// FullWidthSelection), and per-block QTextBlockFormat::setBackground
/// is ignored by QPlainTextDocumentLayout for the inter-block gap —
/// so consecutive `+` lines render as stacked green bands separated
/// by visible white stripes. This subclass paints the colour itself,
/// flush from one block's top to the next block's top, before
/// delegating text drawing to the base class.
///
/// The line-type is detected purely from the first character of each
/// block's text (`+`, `-`, `@@`, leading space, etc.) so the widget
/// is a drop-in replacement that needs no additional state.
class DiffTextEdit : public QPlainTextEdit {
    Q_OBJECT

public:
    explicit DiffTextEdit(QWidget* parent = nullptr);

protected:
    void paintEvent(QPaintEvent* event) override;
};

} // namespace gitbolt::widgets
