#include "widgets/FileTreeWidget.h"

#include "git/Repository.h"
#include "git/Tree.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QTextBlock>
#include <QTextCursor>
#include <QTreeView>
#include <QVBoxLayout>

#include <algorithm>
#include <unordered_map>

namespace gitbolt::widgets {

namespace {
constexpr int kPathRole    = Qt::UserRole + 1;
constexpr int kOidRole     = Qt::UserRole + 2;
constexpr int kIsDirRole   = Qt::UserRole + 3;
constexpr int kSizeRole    = Qt::UserRole + 4;

// Cap individual blob previews to 1 MB. Anything larger is almost
// certainly binary or generated and the user can't usefully scroll
// through it in a small inspector pane anyway.
constexpr quint64 kMaxPreviewBytes = 1 * 1024 * 1024;

// Same single-line-height fix as DiffViewerWidget — without this,
// the file preview pane shows lines with QPlainTextEdit's default
// leading, while the Diff tab shows them packed tight. The two
// inspector tabs read as inconsistent until both apply this.
// Wired up via QTextDocument::contentsChange in setupUi() so every
// setPlainText() call (and there are several — directory listings,
// blob contents, error placeholders) picks it up automatically.
void applySingleLineHeight(QPlainTextEdit* editor)
{
    QTextCursor c(editor->document());
    c.select(QTextCursor::Document);
    QTextBlockFormat fmt;
    fmt.setLineHeight(100, QTextBlockFormat::ProportionalHeight);
    c.mergeBlockFormat(fmt);
}
} // namespace

FileTreeWidget::FileTreeWidget(QWidget* parent)
    : QWidget(parent)
{
    setupUi();
}

void FileTreeWidget::setupUi()
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    splitter_ = new QSplitter(Qt::Horizontal, this);
    splitter_->setChildrenCollapsible(false);

    // ----- Left pane: filter + tree view -----------------------------
    auto* leftPane = new QWidget(splitter_);
    auto* leftLayout = new QVBoxLayout(leftPane);
    leftLayout->setContentsMargins(4, 4, 2, 4);
    leftLayout->setSpacing(4);

    filterInput_ = new QLineEdit(leftPane);
    filterInput_->setObjectName(QStringLiteral("fileTree.filter"));
    filterInput_->setPlaceholderText(tr("Filter files…"));
    filterInput_->setClearButtonEnabled(true);
    leftLayout->addWidget(filterInput_);

    treeView_ = new QTreeView(leftPane);
    treeView_->setObjectName(QStringLiteral("fileTree.view"));
    treeView_->setHeaderHidden(true);
    treeView_->setUniformRowHeights(true);
    treeView_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    treeView_->setContextMenuPolicy(Qt::CustomContextMenu);
    treeView_->setAnimated(false);
    treeView_->setIndentation(16);
    leftLayout->addWidget(treeView_, 1);

    model_ = new QStandardItemModel(this);
    proxy_ = new QSortFilterProxyModel(this);
    proxy_->setSourceModel(model_);
    proxy_->setFilterCaseSensitivity(Qt::CaseInsensitive);
    proxy_->setRecursiveFilteringEnabled(true);  // keep ancestors of matching rows
    treeView_->setModel(proxy_);

    splitter_->addWidget(leftPane);

    // ----- Right pane: header + preview ------------------------------
    auto* rightPane = new QWidget(splitter_);
    auto* rightLayout = new QVBoxLayout(rightPane);
    rightLayout->setContentsMargins(2, 4, 4, 4);
    rightLayout->setSpacing(4);

    previewHeader_ = new QLabel(tr("Select a file to preview its contents"),
                                rightPane);
    previewHeader_->setStyleSheet(QStringLiteral(
        "QLabel { color: #444; padding: 2px; }"));
    previewHeader_->setWordWrap(true);
    rightLayout->addWidget(previewHeader_);

    preview_ = new QPlainTextEdit(rightPane);
    preview_->setObjectName(QStringLiteral("fileTree.preview"));
    preview_->setReadOnly(true);
    // Match DiffViewerWidget's font exactly so the two inspector
    // tabs render at identical line height. Previously this used
    // QFontDatabase::systemFont(FixedFont), which on macOS picks
    // a fixed font at the system's default editor point size —
    // typically larger than the diff viewer's explicit 11 pt
    // Menlo, producing visibly taller rows in the file preview
    // than in the diff. Fixing the font + size in both places is
    // the only reliable way to keep the spacing identical, since
    // line height is driven by the font's own ascent/descent
    // metrics, not by Qt-level layout.
    QFont monoFont(QStringLiteral("Menlo, Consolas, monospace"));
    monoFont.setStyleHint(QFont::Monospace);
    monoFont.setPointSize(11);
    preview_->setFont(monoFont);
    preview_->setLineWrapMode(QPlainTextEdit::NoWrap);
    preview_->setPlaceholderText(tr("(no file selected)"));
    // Reapply single-line-height after every text replacement so
    // freshly inserted content (file body, directory listing,
    // error placeholder) gets the same tight spacing as the diff
    // viewer. contentsChange fires once per setPlainText, and
    // mergeBlockFormat doesn't itself fire contentsChange, so
    // this won't recurse. Editor is read-only so no key-event
    // edits are possible — only setPlainText triggers this.
    connect(preview_->document(), &QTextDocument::contentsChange,
            preview_, [this](int, int, int) {
        applySingleLineHeight(preview_);
    });
    rightLayout->addWidget(preview_, 1);

    splitter_->addWidget(rightPane);
    splitter_->setStretchFactor(0, 2);
    splitter_->setStretchFactor(1, 5);

    outer->addWidget(splitter_);

    connect(treeView_, &QTreeView::clicked,
            this, &FileTreeWidget::onTreeClicked);
    connect(treeView_, &QTreeView::customContextMenuRequested,
            this, &FileTreeWidget::onContextMenu);
    connect(filterInput_, &QLineEdit::textChanged,
            this, &FileTreeWidget::onFilterTextChanged);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void FileTreeWidget::setRepository(git::Repository* repo)
{
    if (repo_ == repo)
        return;
    repo_ = repo;
    clear();
}

void FileTreeWidget::setCommit(const git::ObjectId& commitId)
{
    if (commitId.isZero()) {
        clear();
        return;
    }
    if (commitId == currentCommit_ && model_->rowCount() > 0)
        return;

    currentCommit_ = commitId;
    rebuildTree();
}

void FileTreeWidget::clear()
{
    model_->clear();
    preview_->clear();
    previewHeader_->setText(tr("Select a file to preview its contents"));
    currentCommit_ = git::ObjectId{};
}

// ---------------------------------------------------------------------------
// Tree construction
// ---------------------------------------------------------------------------

void FileTreeWidget::rebuildTree()
{
    model_->clear();
    preview_->clear();
    previewHeader_->setText(tr("Loading file tree…"));

    if (!repo_ || currentCommit_.isZero())
        return;

    auto walk = repo_->walkTreeAtCommit(currentCommit_);
    if (!walk) {
        previewHeader_->setText(tr("Failed to load tree at commit %1")
                                    .arg(QString::fromStdString(
                                        currentCommit_.toShortHex())));
        return;
    }

    const auto& entries = *walk;

    // Sort entries: directories before files at every level, then
    // alphabetical within each group (GitHub / Finder style). We
    // build a component-wise sort key so ordering is consistent
    // both within a single parent and across the whole flat walk:
    //
    //   src/conf/SettingsService.cpp →
    //     [ (DIR, "src"), (DIR, "conf"), (FILE, "SettingsService.cpp") ]
    //
    // Lexicographic comparison of these keys gives the GitHub
    // ordering: at each level a (DIR, name) tuple sorts before any
    // (FILE, name) tuple regardless of name. The previous comparator
    // mixed two different orderings (path-based for cross-parent
    // entries, dir-first for same-parent) and produced a non-
    // transitive comparator — std::sort is undefined under that.
    constexpr int DIR_KEY  = 0;
    constexpr int FILE_KEY = 1;
    using SortKey = std::vector<std::pair<int, std::string>>;
    auto buildSortKey = [&](const git::TreeEntry& e) -> SortKey {
        SortKey key;
        const std::string& p = e.path;
        size_t pos = 0;
        while (true) {
            const size_t slash = p.find('/', pos);
            if (slash == std::string::npos) {
                // Last (own) component — flag matches the entry's
                // own kind: tree → DIR_KEY, blob → FILE_KEY.
                key.emplace_back(e.isTree ? DIR_KEY : FILE_KEY,
                                 p.substr(pos));
                return key;
            }
            // Intermediate component is by definition a parent
            // directory along the path, so always DIR_KEY.
            key.emplace_back(DIR_KEY, p.substr(pos, slash - pos));
            pos = slash + 1;
        }
    };

    auto sorted = entries;
    std::sort(sorted.begin(), sorted.end(),
              [&buildSortKey](const git::TreeEntry& a,
                              const git::TreeEntry& b) {
                  return buildSortKey(a) < buildSortKey(b);
              });

    QFileIconProvider iconProvider;
    const QIcon folderIcon = iconProvider.icon(QFileIconProvider::Folder);
    const QIcon fileIcon   = iconProvider.icon(QFileIconProvider::File);

    // path → item lookup so children can find their parent in O(1)
    // during the second pass. The empty key represents the model
    // root (invisibleRootItem()).
    std::unordered_map<std::string, QStandardItem*> itemByPath;
    itemByPath.reserve(sorted.size() + 1);
    itemByPath[std::string{}] = model_->invisibleRootItem();

    int dirCount  = 0;
    int fileCount = 0;
    quint64 totalBytes = 0;

    for (const auto& entry : sorted) {
        QString name = QString::fromStdString(entry.name);
        QString display = name;
        if (!entry.isTree && entry.size > 0)
            display = QStringLiteral("%1   %2")
                          .arg(name, formatSize(entry.size));

        auto* item = new QStandardItem(display);
        item->setData(QString::fromStdString(entry.path), kPathRole);
        item->setData(QString::fromStdString(entry.oid.toHex()), kOidRole);
        item->setData(entry.isTree, kIsDirRole);
        item->setData(QVariant::fromValue<quint64>(entry.size), kSizeRole);
        item->setEditable(false);
        item->setIcon(entry.isTree ? folderIcon : fileIcon);
        if (entry.isTree) {
            QFont f = item->font();
            f.setBold(true);
            item->setFont(f);
        }

        // Find parent path by trimming the last component.
        std::string parentPath;
        const auto slash = entry.path.rfind('/');
        if (slash != std::string::npos)
            parentPath = entry.path.substr(0, slash);

        auto it = itemByPath.find(parentPath);
        QStandardItem* parent = (it != itemByPath.end())
                                    ? it->second
                                    : model_->invisibleRootItem();
        parent->appendRow(item);

        if (entry.isTree) {
            itemByPath[entry.path] = item;
            ++dirCount;
        } else {
            ++fileCount;
            totalBytes += entry.size;
        }
    }

    // Header summary mirrors GitExtensions' "(N) Tree at <hex>:
    // <files>, <dirs>, <total size>" line.
    previewHeader_->setText(
        tr("%1 files, %2 directories — %3 total")
            .arg(fileCount).arg(dirCount).arg(formatSize(totalBytes)));

    // Auto-expand the root level so users see top-level structure
    // immediately without having to click. Don't recurse — large
    // repos with deep nesting would be unusable.
    treeView_->expandToDepth(0);
}

// ---------------------------------------------------------------------------
// Selection / preview
// ---------------------------------------------------------------------------

void FileTreeWidget::onTreeClicked(const QModelIndex& index)
{
    if (!index.isValid())
        return;
    const QModelIndex sourceIndex = proxy_->mapToSource(index);
    QStandardItem* item = model_->itemFromIndex(sourceIndex);
    if (!item)
        return;

    const bool isDir = item->data(kIsDirRole).toBool();
    if (isDir) {
        // Don't preview directories — show a synthetic listing of
        // their immediate contents like GitExtensions does.
        const QString path = item->data(kPathRole).toString();
        QStringList lines;
        lines << tr("(%1) %2/").arg(item->rowCount()).arg(path);
        lines << QString{};
        for (int i = 0; i < item->rowCount(); ++i) {
            QStandardItem* child = item->child(i);
            if (!child)
                continue;
            const QString childPath = child->data(kPathRole).toString();
            const auto last = childPath.lastIndexOf(QLatin1Char('/'));
            const QString basename = (last >= 0)
                                          ? childPath.mid(last + 1)
                                          : childPath;
            lines << (child->data(kIsDirRole).toBool()
                          ? basename + QLatin1Char('/')
                          : basename);
        }
        preview_->setPlainText(lines.join(QLatin1Char('\n')));
        previewHeader_->setText(path + QStringLiteral(" — directory"));
        return;
    }

    const QString path = item->data(kPathRole).toString();
    const QString hex  = item->data(kOidRole).toString();
    const quint64 sz   = item->data(kSizeRole).value<quint64>();
    const auto blobId  = git::ObjectId::fromHex(hex.toStdString());
    showBlobPreview(blobId, path, sz);
}

void FileTreeWidget::showBlobPreview(const git::ObjectId& blobId,
                                      const QString& path,
                                      quint64 size)
{
    previewHeader_->setText(QStringLiteral("%1 — %2")
                                .arg(path, formatSize(size)));
    preview_->clear();

    if (!repo_) {
        preview_->setPlainText(tr("(no repository loaded)"));
        return;
    }

    if (size > kMaxPreviewBytes) {
        preview_->setPlainText(
            tr("(file is %1 — too large to preview)").arg(formatSize(size)));
        return;
    }

    auto blob = repo_->readBlob(blobId);
    if (!blob) {
        preview_->setPlainText(tr("(failed to read blob)"));
        return;
    }

    const QByteArray bytes(blob->data(), static_cast<int>(blob->size()));

    if (looksBinary(bytes)) {
        preview_->setPlainText(
            tr("(binary file — %1, not previewed)").arg(formatSize(size)));
        return;
    }

    // Decode as UTF-8 for the common case; fall back to Latin-1
    // (which never errors out) so legacy text files still show.
    QString text = QString::fromUtf8(bytes);
    if (text.contains(QChar(0xfffd))) {
        // Replacement character means the UTF-8 decoder couldn't
        // make sense of some bytes — try Latin-1.
        text = QString::fromLatin1(bytes);
    }
    preview_->setPlainText(text);
}

// Sniff the first 8 KB for NUL bytes — git itself uses the same
// heuristic for "is this binary?" before producing a textual diff.
bool FileTreeWidget::looksBinary(const QByteArray& data)
{
    const qsizetype probe = std::min<qsizetype>(data.size(), 8192);
    for (qsizetype i = 0; i < probe; ++i) {
        if (data[i] == '\0')
            return true;
    }
    return false;
}

QString FileTreeWidget::formatSize(quint64 bytes)
{
    constexpr quint64 KB = 1024;
    constexpr quint64 MB = KB * 1024;
    constexpr quint64 GB = MB * 1024;
    if (bytes >= GB) return QStringLiteral("%1 GB").arg(bytes / double(GB), 0, 'f', 1);
    if (bytes >= MB) return QStringLiteral("%1 MB").arg(bytes / double(MB), 0, 'f', 1);
    if (bytes >= KB) return QStringLiteral("%1 KB").arg(bytes / double(KB), 0, 'f', 1);
    return QStringLiteral("%1 B").arg(bytes);
}

// ---------------------------------------------------------------------------
// Context menu / filter
// ---------------------------------------------------------------------------

void FileTreeWidget::onContextMenu(const QPoint& pos)
{
    const QModelIndex idx = treeView_->indexAt(pos);
    if (!idx.isValid())
        return;
    const QModelIndex src = proxy_->mapToSource(idx);
    QStandardItem* item = model_->itemFromIndex(src);
    if (!item)
        return;

    const QString path = item->data(kPathRole).toString();
    const bool isDir   = item->data(kIsDirRole).toBool();

    QMenu menu(this);
    menu.addAction(tr("Copy Path"), this, [path]() {
        QApplication::clipboard()->setText(path);
    });
    menu.addAction(tr("Copy Name"), this, [path]() {
        const auto last = path.lastIndexOf(QLatin1Char('/'));
        QApplication::clipboard()->setText(
            (last >= 0) ? path.mid(last + 1) : path);
    });
    menu.addSeparator();

    if (!isDir) {
        QAction* histAction = menu.addAction(tr("Show History…"));
        connect(histAction, &QAction::triggered, this, [this, path]() {
            emit showHistoryRequested(path);
        });

        QAction* blameAction = menu.addAction(tr("Blame"));
        connect(blameAction, &QAction::triggered, this, [this, path]() {
            emit blameRequested(path);
        });

        QAction* openAction = menu.addAction(tr("Open Externally"));
        connect(openAction, &QAction::triggered, this, [this, path]() {
            emit openExternallyRequested(path);
        });
    } else {
        menu.addAction(tr("Expand All"), this, [this, idx]() {
            treeView_->expandRecursively(idx);
        });
        menu.addAction(tr("Collapse All"), this, [this, idx]() {
            treeView_->collapse(idx);
        });
    }

    menu.exec(treeView_->viewport()->mapToGlobal(pos));
}

void FileTreeWidget::onFilterTextChanged(const QString& text)
{
    proxy_->setFilterFixedString(text);
    if (!text.isEmpty()) {
        // Expand everything so matching descendants are visible.
        treeView_->expandAll();
    } else {
        treeView_->collapseAll();
        treeView_->expandToDepth(0);
    }
}

} // namespace gitbolt::widgets
