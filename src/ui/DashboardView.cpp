#include "ui/DashboardView.h"
#include "conf/SettingsService.h"
#include "services/RecentRepoProbe.h"

#include <QDateTime>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEvent>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QLabel>
#include <QMimeData>
#include <QPointer>
#include <QPushButton>
#include <QTimer>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

namespace gitbolt::ui {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

DashboardView::DashboardView(conf::SettingsService* settings, QWidget* parent)
    : QWidget(parent)
    , settings_(settings)
{
    setAcceptDrops(true);
    setupUi();
    refreshRecentList();

    if (settings_) {
        connect(settings_, &conf::SettingsService::settingsChanged,
                this, &DashboardView::refreshRecentList);
    }
}

// ---------------------------------------------------------------------------
// UI construction
// ---------------------------------------------------------------------------

void DashboardView::setupUi()
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(40, 30, 40, 30);

    // Title row
    auto* titleRow = new QHBoxLayout;
    titleRow->addStretch();
    titleLabel_ = new QLabel(this);
    titleLabel_->setText(
        QStringLiteral("<div style='text-align:center;'>"
                       "<span style='font-size:28pt; font-weight:bold;'>GitBolt</span><br/>"
                       "<span style='font-size:11pt; color:gray;'>Fast cross-platform Git GUI</span>"
                       "</div>"));
    titleLabel_->setAlignment(Qt::AlignCenter);
    titleRow->addWidget(titleLabel_);
    titleRow->addStretch();
    root->addLayout(titleRow);
    root->addSpacing(30);

    // Action cards
    auto* cardsRow = new QHBoxLayout;
    cardsRow->setSpacing(16);
    cardsRow->addStretch();

    // NOTE: emoji must be decoded from UTF-8 bytes, not interpreted as Latin-1.
    // QStringLiteral treats each byte as one QChar, which produces mojibake.
    openBtn_ = createActionCard(tr("Open Repository"),
                                QString::fromUtf8("\xF0\x9F\x93\x82"),  // 📂
                                tr("Open an existing local repository"));
    cloneBtn_ = createActionCard(tr("Clone Repository"),
                                 QString::fromUtf8("\xE2\xAC\x87"),     // ⬇
                                 tr("Clone a remote repository"));
    initBtn_ = createActionCard(tr("Create New Repository"),
                                QString::fromUtf8("\xE2\x9C\xA8"),      // ✨
                                tr("Create a new empty repository"));

    cardsRow->addWidget(openBtn_);
    cardsRow->addWidget(cloneBtn_);
    cardsRow->addWidget(initBtn_);
    cardsRow->addStretch();
    root->addLayout(cardsRow);
    root->addSpacing(24);

    // Recent repos header
    auto* recentHeader = new QHBoxLayout;
    auto* recentTitle = new QLabel(tr("Recent Repositories"), this);
    recentTitle->setStyleSheet(QStringLiteral("font-size: 13pt; font-weight: bold;"));
    recentHeader->addWidget(recentTitle);
    recentHeader->addStretch();
    clearRecentBtn_ = new QPushButton(tr("Clear Recent"), this);
    clearRecentBtn_->setFlat(true);
    recentHeader->addWidget(clearRecentBtn_);
    root->addLayout(recentHeader);

    // Recent repo list — seven-column table. Columns 1-5 are filled in
    // asynchronously by probeAndPopulate() so the dashboard can paint
    // immediately even if one of the recents lives on a slow disk or
    // is a huge repo (status() scan can take a few hundred ms on the
    // likes of kibana / node_modules-heavy trees).
    recentList_ = new QTreeWidget(this);
    recentList_->setColumnCount(7);
    recentList_->setHeaderLabels({
        tr("Name"),
        tr("Branch"),
        tr("Status"),
        QString::fromUtf8("\xE2\x86\x91\xE2\x86\x93"),  // "↑↓"
        tr("Accessed"),
        tr("Committed"),
        tr("Path"),
    });
    recentList_->setRootIsDecorated(false);
    recentList_->setAlternatingRowColors(true);
    recentList_->setSelectionMode(QAbstractItemView::SingleSelection);
    recentList_->setUniformRowHeights(true);
    // Explicit widths, applied in updateRecentColumnWidths(). All
    // Interactive so setColumnWidth works on every column; no column
    // is Stretch because with Stretch mixed in, Qt rebalances the
    // others whenever one is set and our computed proportions get lost.
    for (int c = 0; c < recentList_->columnCount(); ++c)
        recentList_->header()->setSectionResizeMode(c, QHeaderView::Interactive);
    recentList_->header()->setStretchLastSection(false);
    // Watch the tree for resize events so we can re-split on each
    // real size change. DashboardView's own resizeEvent fires before
    // Qt's layout pass propagates the new geometry to recentList_,
    // so its viewport width is stale at that moment.
    recentList_->installEventFilter(this);
    root->addWidget(recentList_, 1);

    // Placeholder when no recent repos
    noRecentLabel_ = new QLabel(tr("No recent repositories.\n\n"
                                   "Open, clone, or init a repository to get started,\n"
                                   "or drag-and-drop a folder here."), this);
    noRecentLabel_->setAlignment(Qt::AlignCenter);
    noRecentLabel_->setStyleSheet(QStringLiteral("color: gray; font-size: 11pt;"));
    noRecentLabel_->setWordWrap(true);
    root->addWidget(noRecentLabel_, 1);

    // Connections
    connect(openBtn_, &QPushButton::clicked, this, [this]() {
        emit openRepositoryRequested(QString());
    });
    connect(cloneBtn_, &QPushButton::clicked, this, &DashboardView::cloneRequested);
    connect(initBtn_, &QPushButton::clicked, this, [this]() {
        emit initRequested(QString());
    });
    connect(clearRecentBtn_, &QPushButton::clicked, this, [this]() {
        if (settings_)
            settings_->clearRecentRepositories();
    });
    // Use itemActivated rather than itemDoubleClicked: itemActivated fires
    // on both double-click AND Enter/Return (and in general respects the
    // user's "activate item" platform preference — single-click on KDE,
    // double-click on macOS). This is what the Qt style guide recommends
    // for "open this item" actions, and it also means keyboard users can
    // open a recent repo with ↓ ↓ Enter.
    connect(recentList_, &QTreeWidget::itemActivated, this,
            [this](QTreeWidgetItem* item, int /*col*/) {
                if (item)
                    emit openRepositoryRequested(item->data(0, Qt::UserRole).toString());
            });
}

QPushButton* DashboardView::createActionCard(const QString& title, const QString& iconText,
                                             const QString& description)
{
    // A composite card: a clickable QPushButton that owns three
    // transparent QLabel children laid out vertically. Using child
    // labels (rather than a single \n-joined button text) lets each
    // element have its own font size — the old approach forced the
    // icon, title, and description to all share the button's 10pt
    // font, which rendered the emoji tiny inside a much larger card.
    //
    // WA_TransparentForMouseEvents on every label ensures clicks and
    // hover events pass through to the button beneath, so the native
    // pressed / hover / focus states all still work.
    auto* btn = new QPushButton(this);
    btn->setFixedSize(290, 220);
    btn->setCursor(Qt::PointingHandCursor);
    btn->setStyleSheet(QStringLiteral(
        "QPushButton {"
        "  border: 1px solid palette(mid);"
        "  border-radius: 10px;"
        "  padding: 0px;"
        "  text-align: center;"
        "}"
        "QPushButton:hover {"
        "  border-color: palette(highlight);"
        "  background: palette(midlight);"
        "}"));

    auto* layout = new QVBoxLayout(btn);
    layout->setContentsMargins(16, 20, 16, 20);
    layout->setSpacing(6);

    auto* iconLabel = new QLabel(iconText, btn);
    iconLabel->setAlignment(Qt::AlignCenter);
    iconLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    // 44pt — dominates the card, "way larger" than the old 10pt the
    // emoji inherited from the button's default font.
    iconLabel->setStyleSheet(QStringLiteral(
        "QLabel { font-size: 44pt; background: transparent; border: none; }"));

    auto* titleLabel = new QLabel(title, btn);
    titleLabel->setAlignment(Qt::AlignCenter);
    titleLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    titleLabel->setStyleSheet(QStringLiteral(
        "QLabel { font-size: 18pt; font-weight: bold;"
        "         background: transparent; border: none; }"));

    auto* descLabel = new QLabel(description, btn);
    descLabel->setAlignment(Qt::AlignCenter);
    descLabel->setWordWrap(true);
    descLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    descLabel->setStyleSheet(QStringLiteral(
        "QLabel { font-size: 13pt; color: palette(mid);"
        "         background: transparent; border: none; }"));

    // Stretch on both ends so the icon/title/description block sits
    // vertically centered within the fixed-height card, rather than
    // being top-aligned with dead space below the description.
    layout->addStretch();
    layout->addWidget(iconLabel);
    layout->addWidget(titleLabel);
    layout->addWidget(descLabel);
    layout->addStretch();

    return btn;
}

// ---------------------------------------------------------------------------
// Recent list refresh
// ---------------------------------------------------------------------------

// Format a QDateTime as a short relative-time phrase ("2m ago", "3d
// ago", "6mo ago"). Invalid/null QDateTimes render as an em-dash so
// an empty cell doesn't look like a rendering bug — it's explicitly
// "no data". Kept as a free function rather than a method because it
// has no DashboardView state and makes the call sites read cleaner.
static QString formatRelativeTime(const QDateTime& when)
{
    if (!when.isValid())
        return QStringLiteral("—");
    const qint64 secs = when.secsTo(QDateTime::currentDateTimeUtc());
    if (secs < 0)          return DashboardView::tr("just now");
    if (secs < 60)         return DashboardView::tr("%1s ago").arg(secs);
    if (secs < 3600)       return DashboardView::tr("%1m ago").arg(secs / 60);
    if (secs < 86400)      return DashboardView::tr("%1h ago").arg(secs / 3600);
    if (secs < 30LL * 86400) return DashboardView::tr("%1d ago").arg(secs / 86400);
    if (secs < 365LL * 86400) return DashboardView::tr("%1mo ago").arg(secs / (30LL * 86400));
    return DashboardView::tr("%1y ago").arg(secs / (365LL * 86400));
}

void DashboardView::refreshRecentList()
{
    recentList_->clear();
    QStringList repos = settings_ ? settings_->recentRepositories() : QStringList{};
    const QHash<QString, QDateTime> accessedAt =
        settings_ ? settings_->recentAccessTimes() : QHash<QString, QDateTime>{};

    bool hasRecent = !repos.isEmpty();
    recentList_->setVisible(hasRecent);
    clearRecentBtn_->setVisible(hasRecent);
    noRecentLabel_->setVisible(!hasRecent);

    // Ellipsis placeholder for the async-populated columns. The probe
    // usually completes within ~100ms for typical repos, but showing
    // something here avoids a "table with blanks that fill in later"
    // flicker for a moment.
    const QString pending = QStringLiteral("…");

    for (const QString& path : repos) {
        QFileInfo fi(path);
        auto* item = new QTreeWidgetItem(recentList_);
        item->setText(0, fi.fileName());
        item->setText(1, pending);                  // Branch
        item->setText(2, pending);                  // Status
        item->setText(3, pending);                  // ↑↓
        item->setText(4, formatRelativeTime(accessedAt.value(path)));
        item->setText(5, pending);                  // Committed
        item->setText(6, fi.absoluteFilePath());
        // Stash the canonical path on column 0 so itemActivated can
        // retrieve it regardless of which column the user clicked in.
        item->setData(0, Qt::UserRole, path);
        for (int c = 0; c < recentList_->columnCount(); ++c)
            item->setToolTip(c, path);

        probeAndPopulate(item, path);
    }

    updateRecentColumnWidths();
}

void DashboardView::probeAndPopulate(QTreeWidgetItem* row, const QString& path)
{
    // Capture a QPointer to `this` so the completion handler is a no-op
    // if DashboardView is destroyed before the probe finishes. The row
    // is identified by its stashed path (UserRole on column 0) rather
    // than by pointer — if refreshRecentList() runs again and rebuilds
    // the tree, the original QTreeWidgetItem* is dangling, but a new
    // row with the same path has the same UserRole and receives the
    // (still relevant) data. We look up the row by walking the tree's
    // top-level items in the handler.
    auto watcher = new QFutureWatcher<services::RecentRepoInfo>(this);
    QObject::connect(watcher, &QFutureWatcher<services::RecentRepoInfo>::finished,
                     this, [this, watcher, path, row]() {
        watcher->deleteLater();

        // Locate the row for `path` by scanning current top-level
        // items. If the tree has since been rebuilt, `row` may be
        // dangling — never dereference it; we only use it as a hint
        // if it's still present (cheap common case).
        QTreeWidgetItem* target = nullptr;
        for (int i = 0; i < recentList_->topLevelItemCount(); ++i) {
            QTreeWidgetItem* it = recentList_->topLevelItem(i);
            if (it == row || it->data(0, Qt::UserRole).toString() == path) {
                target = it;
                break;
            }
        }
        if (!target)
            return;

        const services::RecentRepoInfo info = watcher->result();

        // Column 1: Branch
        if (!info.valid)
            target->setText(1, QStringLiteral("—"));
        else if (info.detachedHead)
            target->setText(1, tr("(detached)"));
        else
            target->setText(1, info.branch);

        // Column 2: Status (dirty count). Blank when clean keeps the
        // common case visually quiet; a leading bullet flags the
        // presence of uncommitted work at a glance.
        if (!info.valid)
            target->setText(2, QStringLiteral("—"));
        else if (info.dirtyCount == 0)
            target->setText(2, QString{});
        else
            target->setText(2, QString::fromUtf8("\xE2\x97\x8F ") +     // "● "
                                   QString::number(info.dirtyCount));

        // Column 3: ahead/behind. No upstream → em-dash; with upstream
        // but zero drift → "=" (in-sync). Otherwise "↑N ↓M", omitting
        // either arrow if its count is zero.
        if (!info.valid) {
            target->setText(3, QStringLiteral("—"));
        } else if (!info.hasUpstream) {
            target->setText(3, QStringLiteral("—"));
        } else if (info.ahead == 0 && info.behind == 0) {
            target->setText(3, QStringLiteral("="));
        } else {
            QStringList parts;
            if (info.ahead)
                parts << QString::fromUtf8("\xE2\x86\x91") +             // "↑"
                             QString::number(info.ahead);
            if (info.behind)
                parts << QString::fromUtf8("\xE2\x86\x93") +             // "↓"
                             QString::number(info.behind);
            target->setText(3, parts.join(QLatin1Char(' ')));
        }

        // Column 5: last committed. (Column 4 "Accessed" is already
        // populated synchronously in refreshRecentList from settings.)
        if (!info.valid)
            target->setText(5, QStringLiteral("—"));
        else
            target->setText(5, formatRelativeTime(info.lastCommit));
    });

    // Kick off the probe on Qt's global thread pool. QtConcurrent::run
    // is the most lightweight "do this on a worker" primitive; we don't
    // need a custom thread pool since probes are short and capped by
    // the recent list length (≤10).
    watcher->setFuture(QtConcurrent::run(&services::probeRecentRepo, path));
}

// ---------------------------------------------------------------------------
// Column width sizing
// ---------------------------------------------------------------------------

void DashboardView::updateRecentColumnWidths()
{
    if (!recentList_)
        return;

    // Proportions chosen to keep the high-signal columns visible
    // without horizontal scroll at typical window widths:
    //   Name 15% | Branch 12% | Status 7% | ↑↓ 7% | Accessed 11% | Committed 11% | Path 37%
    // They sum to 100. If anyone tweaks one, rebalance the others so
    // the sum stays at 100, otherwise the last column picks up the
    // slack / overflow below.
    static constexpr int kPercents[] = { 15, 12, 7, 7, 11, 11, 37 };
    static_assert(sizeof(kPercents) / sizeof(kPercents[0]) == 7,
                  "kPercents must stay in sync with the tree's column count");

    const int total = recentList_->viewport()->width();
    if (total <= 0)
        return;  // first paint; resizeEvent will run again once laid out

    int assigned = 0;
    for (int c = 0; c < 6; ++c) {
        const int w = total * kPercents[c] / 100;
        recentList_->setColumnWidth(c, w);
        assigned += w;
    }
    // Give the last column whatever's left so rounding doesn't leave
    // a 1-2px gap or overflow — this way columns always fill the
    // viewport exactly.
    recentList_->setColumnWidth(6, qMax(50, total - assigned));
}

bool DashboardView::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == recentList_ && event->type() == QEvent::Resize) {
        // Defer to the next tick so the tree's internal layout (header,
        // viewport, scrollbars) has fully settled. Running synchronously
        // here yielded stale viewport widths and the 30% came out as 30%
        // of some pre-layout size instead of the visible size.
        QTimer::singleShot(0, this, &DashboardView::updateRecentColumnWidths);
    }
    return QWidget::eventFilter(watched, event);
}

// ---------------------------------------------------------------------------
// Drag and drop
// ---------------------------------------------------------------------------

void DashboardView::dragEnterEvent(QDragEnterEvent* event)
{
    if (event->mimeData()->hasUrls()) {
        for (const QUrl& url : event->mimeData()->urls()) {
            if (url.isLocalFile() && QFileInfo(url.toLocalFile()).isDir()) {
                event->acceptProposedAction();
                return;
            }
        }
    }
}

void DashboardView::dropEvent(QDropEvent* event)
{
    for (const QUrl& url : event->mimeData()->urls()) {
        if (url.isLocalFile()) {
            QString path = url.toLocalFile();
            if (QFileInfo(path).isDir()) {
                emit openRepositoryRequested(path);
                return;
            }
        }
    }
}

} // namespace gitbolt::ui
