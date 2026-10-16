#pragma once

#include <QColor>

class QPalette;

namespace gitbolt::editor {

/// The diff viewer's colours for one theme: the full-line stripes
/// DiffTextEdit paints behind added, deleted and hunk-header lines,
/// and the text colours DiffSyntaxHighlighter gives them.
struct DiffColors {
    QColor addBg;
    QColor delBg;
    QColor hunkBg;
    QColor addFg;
    QColor delFg;
    QColor hunkFg;
    QColor contextFg;
};

/// The colours for `palette`'s theme: dark when its window text is
/// lighter than its window (RepoOperationBar's test), else light.
/// The light swatches alone left the dark theme with pale stripes
/// under its pale text (1.2:1 in the side-by-side panes, which have
/// no highlighter) and a red that read at 2.5:1; the dark ones follow
/// GitHub's dark diff and keep every pairing above 6:1.
DiffColors diffColorsFor(const QPalette& palette);

} // namespace gitbolt::editor
