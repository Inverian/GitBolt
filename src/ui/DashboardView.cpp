#include "ui/DashboardView.h"
#include "conf/SettingsService.h"

#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMimeData>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

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
    initBtn_ = createActionCard(tr("Init New Repository"),
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

    // Recent repo list
    recentList_ = new QListWidget(this);
    recentList_->setAlternatingRowColors(true);
    recentList_->setSpacing(2);
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
    connect(recentList_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
        if (item)
            emit openRepositoryRequested(item->data(Qt::UserRole).toString());
    });
}

QPushButton* DashboardView::createActionCard(const QString& title, const QString& iconText,
                                             const QString& description)
{
    auto* btn = new QPushButton(this);
    btn->setFixedSize(220, 140);
    btn->setCursor(Qt::PointingHandCursor);
    btn->setText(QStringLiteral("%1\n%2\n%3").arg(iconText, title, description));
    btn->setStyleSheet(QStringLiteral(
        "QPushButton {"
        "  border: 1px solid palette(mid);"
        "  border-radius: 8px;"
        "  padding: 16px;"
        "  font-size: 10pt;"
        "  text-align: center;"
        "}"
        "QPushButton:hover {"
        "  border-color: palette(highlight);"
        "  background: palette(midlight);"
        "}"));
    return btn;
}

// ---------------------------------------------------------------------------
// Recent list refresh
// ---------------------------------------------------------------------------

void DashboardView::refreshRecentList()
{
    recentList_->clear();
    QStringList repos = settings_ ? settings_->recentRepositories() : QStringList{};

    bool hasRecent = !repos.isEmpty();
    recentList_->setVisible(hasRecent);
    clearRecentBtn_->setVisible(hasRecent);
    noRecentLabel_->setVisible(!hasRecent);

    for (const QString& path : repos) {
        QFileInfo fi(path);
        QString label = QStringLiteral("%1  —  %2").arg(fi.fileName(), fi.absoluteFilePath());
        auto* item = new QListWidgetItem(label, recentList_);
        item->setData(Qt::UserRole, path);
        item->setToolTip(path);
    }
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
