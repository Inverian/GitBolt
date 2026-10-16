#pragma once

#include <QSyntaxHighlighter>
#include <QTextCharFormat>

namespace gitbolt::editor {

struct DiffColors;

/// Syntax highlighter for unified diff text.
///
/// Sets text colours only (DiffTextEdit paints the line backgrounds),
/// in the current theme's DiffColors:
///   - "+" addition lines:      green
///   - "-" deletion lines:      red
///   - "@@" hunk headers:       blue
///   - "diff --git" file headers: bold
///   - Context lines:           gray
class DiffSyntaxHighlighter : public QSyntaxHighlighter {
    Q_OBJECT

public:
    /// Starts with the application palette's colours.
    explicit DiffSyntaxHighlighter(QTextDocument* parent = nullptr);

    /// Recolours the document, for a theme switch. The formats used
    /// to be fixed at construction, so a dark theme kept the light
    /// theme's text colours for good.
    void setColors(const DiffColors& colors);

protected:
    void highlightBlock(const QString& text) override;

private:
    void applyColors(const DiffColors& colors);

    QTextCharFormat additionFmt_;
    QTextCharFormat deletionFmt_;
    QTextCharFormat hunkHeaderFmt_;
    QTextCharFormat fileHeaderFmt_;
    QTextCharFormat contextFmt_;
};

} // namespace gitbolt::editor
