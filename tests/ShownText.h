#pragma once
//
// ShownText — what a label or a tooltip puts on the screen, as text: its
// markup rendered away when Qt takes it for rich text, as it is when
// not. A QLabel and QToolTip decide as Qt::mightBeRichText() says for
// AutoText (a tooltip is always AutoText); toPlainText() of the rendered
// document is what a reader sees, entities decoded.
//
// Tests of text that comes from a repository, a remote or git check that
// what is shown is that text, character for character: "<b>MARKER</b>"
// shown as such, not "MARKER" in bold.
//

#include <QLabel>
#include <QString>
#include <QTextDocument>

namespace gitbolt::test {

inline QString shownText(const QString& text, Qt::TextFormat format)
{
    const bool rich = format == Qt::RichText
                      || (format == Qt::AutoText && Qt::mightBeRichText(text));
    if (!rich)
        return text;
    QTextDocument document;
    document.setHtml(text);
    return document.toPlainText();
}

inline QString shownText(const QLabel* label)
{
    return shownText(label->text(), label->textFormat());
}

inline QString shownToolTip(const QString& tip)
{
    return shownText(tip, Qt::AutoText);
}

} // namespace gitbolt::test
