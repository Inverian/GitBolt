#include "widgets/DiffViewerWidget.h"
#include "editor/DiffSyntaxHighlighter.h"

#include <QAction>
#include <QActionGroup>
#include <QLabel>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QSplitter>
#include <QTextBlock>
#include <QToolBar>
#include <QVBoxLayout>

namespace gitbolt::widgets {

// ---------------------------------------------------------------------------
// Color constants
// ---------------------------------------------------------------------------

namespace {
const QColor kAddBg(200, 255, 200);
const QColor kDelBg(255, 220, 220);
const QColor kHunkBg(220, 230, 255);
} // anonymous namespace

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
    unifiedEditor_ = new QPlainTextEdit(this);
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

    leftEditor_ = new QPlainTextEdit(this);
    leftEditor_->setReadOnly(true);
    leftEditor_->setLineWrapMode(QPlainTextEdit::NoWrap);
    leftEditor_->setFont(monoFont);

    rightEditor_ = new QPlainTextEdit(this);
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

    QStringList lines;
    // File header
    lines << QStringLiteral("diff --git a/%1 b/%2")
                 .arg(QString::fromStdString(file.oldPath),
                      QString::fromStdString(file.newPath));

    for (const auto& hunk : file.hunks) {
        lines << QString::fromStdString(hunk.header);
        for (const auto& line : hunk.lines) {
            using LT = gitbolt::git::DiffLineType;
            switch (line.type) {
            case LT::Addition:
                lines << QStringLiteral("+") + QString::fromStdString(line.content);
                break;
            case LT::Deletion:
                lines << QStringLiteral("-") + QString::fromStdString(line.content);
                break;
            case LT::HunkHeader:
                lines << QString::fromStdString(line.content);
                break;
            case LT::FileHeader:
                lines << QString::fromStdString(line.content);
                break;
            default:
                lines << QStringLiteral(" ") + QString::fromStdString(line.content);
                break;
            }
        }
    }

    unifiedEditor_->setPlainText(lines.join(QLatin1Char('\n')));
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

    QStringList leftLines;
    QStringList rightLines;

    // We also need to colour the backgrounds ourselves since the side panels
    // do not use the DiffSyntaxHighlighter.
    struct LineStyle {
        QColor bg;
    };
    std::vector<LineStyle> leftStyles;
    std::vector<LineStyle> rightStyles;

    for (const auto& hunk : file.hunks) {
        // Hunk header on both sides
        leftLines  << QString::fromStdString(hunk.header);
        rightLines << QString::fromStdString(hunk.header);
        leftStyles.push_back({kHunkBg});
        rightStyles.push_back({kHunkBg});

        for (const auto& line : hunk.lines) {
            using LT = gitbolt::git::DiffLineType;
            const QString content = QString::fromStdString(line.content);

            switch (line.type) {
            case LT::Context:
            case LT::ContextEOFNL:
                leftLines  << QStringLiteral(" ") + content;
                rightLines << QStringLiteral(" ") + content;
                leftStyles.push_back({QColor()});
                rightStyles.push_back({QColor()});
                break;
            case LT::Deletion:
            case LT::DelEOFNL:
                leftLines  << QStringLiteral("-") + content;
                rightLines << QString();
                leftStyles.push_back({kDelBg});
                rightStyles.push_back({QColor()});
                break;
            case LT::Addition:
            case LT::AddEOFNL:
                leftLines  << QString();
                rightLines << QStringLiteral("+") + content;
                leftStyles.push_back({QColor()});
                rightStyles.push_back({kAddBg});
                break;
            default:
                leftLines  << content;
                rightLines << content;
                leftStyles.push_back({QColor()});
                rightStyles.push_back({QColor()});
                break;
            }
        }
    }

    leftEditor_->setPlainText(leftLines.join(QLatin1Char('\n')));
    rightEditor_->setPlainText(rightLines.join(QLatin1Char('\n')));

    // Apply background colours block-by-block
    auto applyBg = [](QPlainTextEdit* editor, const std::vector<LineStyle>& styles) {
        QTextBlock block = editor->document()->begin();
        size_t idx = 0;
        while (block.isValid() && idx < styles.size()) {
            if (styles[idx].bg.isValid()) {
                QTextCursor cursor(block);
                QTextBlockFormat fmt = block.blockFormat();
                fmt.setBackground(styles[idx].bg);
                cursor.setBlockFormat(fmt);
            }
            block = block.next();
            ++idx;
        }
    };

    applyBg(leftEditor_, leftStyles);
    applyBg(rightEditor_, rightStyles);
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
