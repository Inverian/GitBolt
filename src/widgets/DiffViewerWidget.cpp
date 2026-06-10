#include "widgets/DiffViewerWidget.h"
#include "widgets/DiffTextEdit.h"
#include "editor/DiffSyntaxHighlighter.h"

#include <QAction>
#include <QActionGroup>
#include <QLabel>
#include <QScrollBar>
#include <QSplitter>
#include <QTextBlock>
#include <QToolBar>
#include <QVBoxLayout>

namespace gitbolt::widgets {

// ---------------------------------------------------------------------------
// Background painting note
// ---------------------------------------------------------------------------
//
// Full-line-height background fills for `+` / `-` / `@@` lines are
// handled inside the DiffTextEdit subclass via paintEvent — see
// widgets/DiffTextEdit.cpp. That avoids the inter-block "white
// stripe" you get from QTextCharFormat::setBackground (paints under
// the chars only), QTextBlockFormat::setBackground (ignored across
// the inter-block leading by QPlainTextDocumentLayout), and
// QTextEdit::ExtraSelection + FullWidthSelection (only fills the
// text-line rect, not the leading). The colour swatches live there;
// this file no longer needs its own copies.

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

DiffViewerWidget::DiffViewerWidget(QWidget* parent)
    : QWidget(parent)
{
    setupUi();
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

void DiffViewerWidget::setDiff(const gitbolt::git::DiffResult& diff, int fileIndex)
{
    if (fileIndex < 0 || fileIndex >= static_cast<int>(diff.files.size())) {
        clear();
        return;
    }
    setDiffForFile(diff.files[static_cast<size_t>(fileIndex)]);
}

void DiffViewerWidget::setDiffForFile(const gitbolt::git::DiffFileEntry& file)
{
    currentFile_ = file;
    hasFile_ = true;

    const QString fname = QString::fromStdString(file.path());
    fileLabel_->setText(fname);

    if (mode_ == Unified)
        renderUnified(file);
    else
        renderSideBySide(file);
}

void DiffViewerWidget::clear()
{
    hasFile_ = false;
    currentFile_ = {};
    fileLabel_->setText(QString());
    unifiedEditor_->clear();
    leftEditor_->clear();
    rightEditor_->clear();
}

// ---------------------------------------------------------------------------
// View mode
// ---------------------------------------------------------------------------

void DiffViewerWidget::setViewMode(ViewMode mode)
{
    if (mode_ == mode)
        return;
    mode_ = mode;
    applyViewMode();

    // Re-render current diff in the new mode
    if (hasFile_)
        setDiffForFile(currentFile_);
}

// ---------------------------------------------------------------------------
// Scroll sync
// ---------------------------------------------------------------------------

void DiffViewerWidget::syncScrollLeft(int value)
{
    if (syncingScroll_)
        return;
    syncingScroll_ = true;
    rightEditor_->verticalScrollBar()->setValue(value);
    syncingScroll_ = false;
}

void DiffViewerWidget::syncScrollRight(int value)
{
    if (syncingScroll_)
        return;
    syncingScroll_ = true;
    leftEditor_->verticalScrollBar()->setValue(value);
    syncingScroll_ = false;
}

// ---------------------------------------------------------------------------
// UI setup
// ---------------------------------------------------------------------------

void DiffViewerWidget::setupUi()
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // ---- Toolbar ----------------------------------------------------------
    toolbar_ = new QToolBar(this);
    toolbar_->setIconSize(QSize(16, 16));

    auto* modeGroup = new QActionGroup(this);
    modeGroup->setExclusive(true);

    unifiedAct_ = toolbar_->addAction(tr("Unified"));
    unifiedAct_->setCheckable(true);
    unifiedAct_->setChecked(true);
    modeGroup->addAction(unifiedAct_);

    sideBySideAct_ = toolbar_->addAction(tr("Side-by-Side"));
    sideBySideAct_->setCheckable(true);
    modeGroup->addAction(sideBySideAct_);

    toolbar_->addSeparator();

    fileLabel_ = new QLabel(this);
    fileLabel_->setStyleSheet(QStringLiteral("padding: 0 6px; color: #555;"));
    toolbar_->addWidget(fileLabel_);

    layout->addWidget(toolbar_);

    connect(unifiedAct_, &QAction::triggered, this, [this]() {
        setViewMode(Unified);
    });
    connect(sideBySideAct_, &QAction::triggered, this, [this]() {
        setViewMode(SideBySide);
    });

    // ---- Unified editor ---------------------------------------------------
    unifiedEditor_ = new DiffTextEdit(this);
    unifiedEditor_->setReadOnly(true);
    unifiedEditor_->setLineWrapMode(QPlainTextEdit::NoWrap);
    QFont monoFont(QStringLiteral("Menlo, Consolas, monospace"));
    monoFont.setStyleHint(QFont::Monospace);
    monoFont.setPointSize(11);
    unifiedEditor_->setFont(monoFont);

    unifiedHighlighter_ = new editor::DiffSyntaxHighlighter(
        unifiedEditor_->document());

    layout->addWidget(unifiedEditor_);

    // ---- Side-by-side editors ---------------------------------------------
    sideSplitter_ = new QSplitter(Qt::Horizontal, this);

    leftEditor_ = new DiffTextEdit(this);
    leftEditor_->setReadOnly(true);
    leftEditor_->setLineWrapMode(QPlainTextEdit::NoWrap);
    leftEditor_->setFont(monoFont);

    rightEditor_ = new DiffTextEdit(this);
    rightEditor_->setReadOnly(true);
    rightEditor_->setLineWrapMode(QPlainTextEdit::NoWrap);
    rightEditor_->setFont(monoFont);

    sideSplitter_->addWidget(leftEditor_);
    sideSplitter_->addWidget(rightEditor_);
    sideSplitter_->setStretchFactor(0, 1);
    sideSplitter_->setStretchFactor(1, 1);

    layout->addWidget(sideSplitter_);

    // Synchronized scrolling
    connect(leftEditor_->verticalScrollBar(), &QScrollBar::valueChanged,
            this, &DiffViewerWidget::syncScrollLeft);
    connect(rightEditor_->verticalScrollBar(), &QScrollBar::valueChanged,
            this, &DiffViewerWidget::syncScrollRight);

    // Also sync horizontal scrolling
    connect(leftEditor_->horizontalScrollBar(), &QScrollBar::valueChanged,
            rightEditor_->horizontalScrollBar(), &QScrollBar::setValue);
    connect(rightEditor_->horizontalScrollBar(), &QScrollBar::valueChanged,
            leftEditor_->horizontalScrollBar(), &QScrollBar::setValue);

    // Default to unified mode
    applyViewMode();
}

// ---------------------------------------------------------------------------
// Rendering -- unified
// ---------------------------------------------------------------------------

void DiffViewerWidget::renderUnified(const gitbolt::git::DiffFileEntry& file)
{
    unifiedEditor_->clear();

    if (file.isBinary) {
        unifiedEditor_->setPlainText(tr("Binary file differs"));
        return;
    }

    // libgit2 returns line.content with the source file's trailing
    // newline still attached. If we leave it in and then join with
    // '\n' before setPlainText, every diff line becomes "+text\n\n"
    // and Qt creates an *empty* block between every real one — so
    // every other row in the editor is uncoloured, producing the
    // visible white "gap" between consecutive `+` / `-` lines that
    // the painter then can't fill (those gap blocks don't carry a
    // diff prefix). Strip the embedded newline once at ingest.
    auto stripNewline = [](const std::string& s) {
        QString q = QString::fromStdString(s);
        while (q.endsWith(QLatin1Char('\n')) || q.endsWith(QLatin1Char('\r')))
            q.chop(1);
        return q;
    };

    QStringList lines;
    // File header
    lines << QStringLiteral("diff --git a/%1 b/%2")
                 .arg(QString::fromStdString(file.oldPath),
                      QString::fromStdString(file.newPath));

    for (const auto& hunk : file.hunks) {
        lines << stripNewline(hunk.header);
        for (const auto& line : hunk.lines) {
            using LT = gitbolt::git::DiffLineType;
            const QString content = stripNewline(line.content);
            switch (line.type) {
            case LT::Addition:
                lines << QStringLiteral("+") + content;
                break;
            case LT::Deletion:
                lines << QStringLiteral("-") + content;
                break;
            case LT::HunkHeader:
                lines << content;
                break;
            case LT::FileHeader:
                lines << content;
                break;
            default:
                lines << QStringLiteral(" ") + content;
                break;
            }
        }
    }

    unifiedEditor_->setPlainText(lines.join(QLatin1Char('\n')));
    // Background colours are painted edge-to-edge (no inter-block
    // white slivers) by DiffTextEdit::paintEvent based on each
    // block's leading character — see widgets/DiffTextEdit.cpp.
    // The DiffSyntaxHighlighter still runs over the document to set
    // foreground colours, font weights, etc.
}

// ---------------------------------------------------------------------------
// Rendering -- side-by-side
// ---------------------------------------------------------------------------

void DiffViewerWidget::renderSideBySide(const gitbolt::git::DiffFileEntry& file)
{
    leftEditor_->clear();
    rightEditor_->clear();

    if (file.isBinary) {
        leftEditor_->setPlainText(tr("Binary file"));
        rightEditor_->setPlainText(tr("Binary file"));
        return;
    }

    // Same trailing-newline-strip as renderUnified — line.content
    // carries the source's '\n' which would create empty blocks
    // between every visible row when we setPlainText.
    auto stripNewline = [](const std::string& s) {
        QString q = QString::fromStdString(s);
        while (q.endsWith(QLatin1Char('\n')) || q.endsWith(QLatin1Char('\r')))
            q.chop(1);
        return q;
    };

    QStringList leftLines;
    QStringList rightLines;

    // Colours are painted by DiffTextEdit::paintEvent based on each
    // block's leading character. We just need to make sure the
    // prefix character matches what the painter looks for: `+` for
    // additions on the right pane, `-` for deletions on the left
    // pane, `@@` for hunk headers on both, and a leading space (or
    // empty placeholder line) for everything else.
    for (const auto& hunk : file.hunks) {
        // Hunk header on both sides — `@@`-prefixed text triggers
        // the blue stripe in DiffTextEdit on each pane.
        const QString hdr = stripNewline(hunk.header);
        leftLines  << hdr;
        rightLines << hdr;

        for (const auto& line : hunk.lines) {
            using LT = gitbolt::git::DiffLineType;
            const QString content = stripNewline(line.content);

            switch (line.type) {
            case LT::Context:
            case LT::ContextEOFNL:
                leftLines  << QStringLiteral(" ") + content;
                rightLines << QStringLiteral(" ") + content;
                break;
            case LT::Deletion:
            case LT::DelEOFNL:
                leftLines  << QStringLiteral("-") + content;
                rightLines << QString();
                break;
            case LT::Addition:
            case LT::AddEOFNL:
                leftLines  << QString();
                rightLines << QStringLiteral("+") + content;
                break;
            default:
                leftLines  << content;
                rightLines << content;
                break;
            }
        }
    }

    leftEditor_->setPlainText(leftLines.join(QLatin1Char('\n')));
    rightEditor_->setPlainText(rightLines.join(QLatin1Char('\n')));
}

// ---------------------------------------------------------------------------
// Show/hide panels based on mode
// ---------------------------------------------------------------------------

void DiffViewerWidget::applyViewMode()
{
    const bool unified = (mode_ == Unified);
    unifiedEditor_->setVisible(unified);
    sideSplitter_->setVisible(!unified);

    unifiedAct_->setChecked(unified);
    sideBySideAct_->setChecked(!unified);
}

} // namespace gitbolt::widgets
