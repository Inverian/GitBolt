#pragma once

#include <QSyntaxHighlighter>
#include <QTextCharFormat>

namespace gitbolt::editor {

/// Syntax highlighter for unified diff text.
///
/// Applies foreground + background colours:
///   - "+" addition lines:      green text on light-green background
///   - "-" deletion lines:      red text on light-red background
///   - "@@" hunk headers:       blue foreground
///   - "diff --git" file headers: bold
///   - Context lines:           gray foreground
class DiffSyntaxHighlighter : public QSyntaxHighlighter {
    Q_OBJECT

public:
    explicit DiffSyntaxHighlighter(QTextDocument* parent = nullptr);

protected:
    void highlightBlock(const QString& text) override;

private:
    QTextCharFormat additionFmt_;
    QTextCharFormat deletionFmt_;
    QTextCharFormat hunkHeaderFmt_;
    QTextCharFormat fileHeaderFmt_;
    QTextCharFormat contextFmt_;
};

} // namespace gitbolt::editor
