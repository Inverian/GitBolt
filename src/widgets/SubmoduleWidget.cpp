#include "widgets/SubmoduleWidget.h"

#include "util/ToolTip.h"

#include <QApplication>
#include <QClipboard>
#include <QFont>
#include <QHeaderView>
#include <QTableView>
#include <QToolBar>
#include <QMenu>
#include <QVBoxLayout>

namespace gitbolt::widgets {

// ===========================================================================
// SubmoduleTableModel
// ===========================================================================

SubmoduleTableModel::SubmoduleTableModel(QObject* parent)
    : QAbstractTableModel(parent) {}

int SubmoduleTableModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid())
        return 0;
    return static_cast<int>(submodules_.size());
}

int SubmoduleTableModel::columnCount(const QModelIndex& parent) const {
    if (parent.isValid())
        return 0;
    return ColumnCount;
}

QVariant SubmoduleTableModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid())
        return {};

    const int row = index.row();
    if (row < 0 || row >= static_cast<int>(submodules_.size()))
        return {};

    const auto& sub = submodules_[static_cast<size_t>(row)];

    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case ColName:   return QString::fromStdString(sub.name);
        case ColPath:   return QString::fromStdString(sub.path);
        case ColURL:    return QString::fromStdString(sub.url);
        case ColStatus: return statusString(sub.status);
        default:        return {};
        }
    }

    if (role == Qt::ToolTipRole) {
        // .gitmodules' name, path and URL, as a clone brought them:
        // plain text. In one arg(), so a "%2" in the name stays.
        return util::plainToolTip(QStringLiteral("%1\nPath: %2\nURL: %3\nStatus: %4")
                                      .arg(QString::fromStdString(sub.name),
                                           QString::fromStdString(sub.path),
                                           QString::fromStdString(sub.url),
                                           statusString(sub.status)));
    }

    // Colored status indicator
    if (role == Qt::ForegroundRole && index.column() == ColStatus) {
        // The dark theme's own where the light theme's read at 1.9:1 to
        // 3.4:1 on its rows (#191919, #252525): 4.5:1 or more there.
        const QPalette app = QGuiApplication::palette();
        const bool dark = app.color(QPalette::WindowText).lightness()
                          > app.color(QPalette::Window).lightness();
        switch (sub.status) {
        case git::SubmoduleStatus::Clean:
            return dark ? QColor(0x3f, 0xb9, 0x50) : QColor(0x2e, 0x7d, 0x32);   // green
        case git::SubmoduleStatus::Dirty:
        case git::SubmoduleStatus::Modified:
            return QColor(0xf9, 0xa8, 0x25);   // yellow / amber
        case git::SubmoduleStatus::Uninitialized:
            return QColor(0x9e, 0x9e, 0x9e);   // gray
        case git::SubmoduleStatus::OutOfDate:
            return dark ? QColor(0xff, 0x8a, 0x3d) : QColor(0xe6, 0x51, 0x00);   // orange
        case git::SubmoduleStatus::Added:
            return dark ? QColor(0x56, 0xd3, 0x64) : QColor(0x1b, 0x5e, 0x20);   // green
        case git::SubmoduleStatus::Deleted:
            return dark ? QColor(0xff, 0x7b, 0x72) : QColor(0xc6, 0x28, 0x28);   // red
        }
        return {};
    }

    if (role == Qt::FontRole && index.column() == ColStatus) {
        QFont font;
        font.setBold(true);
        return font;
    }

    // Custom roles
    if (role == SubmoduleNameRole)
        return QString::fromStdString(sub.name);
    if (role == SubmodulePathRole)
        return QString::fromStdString(sub.path);
    if (role == SubmoduleURLRole)
        return QString::fromStdString(sub.url);
    if (role == SubmoduleStatusRole)
        return static_cast<int>(sub.status);

    return {};
}

QVariant SubmoduleTableModel::headerData(int section, Qt::Orientation orientation,
                                          int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};

    switch (section) {
    case ColName:   return tr("Name");
    case ColPath:   return tr("Path");
    case ColURL:    return tr("URL");
    case ColStatus: return tr("Status");
    default:        return {};
    }
}

void SubmoduleTableModel::setSubmodules(std::vector<git::SubmoduleInfo> submodules) {
    beginResetModel();
    submodules_ = std::move(submodules);
    endResetModel();
}

void SubmoduleTableModel::clear() {
    beginResetModel();
    submodules_.clear();
    endResetModel();
}

QString SubmoduleTableModel::nameAtRow(int row) const {
    if (row >= 0 && row < static_cast<int>(submodules_.size()))
        return QString::fromStdString(submodules_[static_cast<size_t>(row)].name);
    return {};
}

QString SubmoduleTableModel::pathAtRow(int row) const {
    if (row >= 0 && row < static_cast<int>(submodules_.size()))
        return QString::fromStdString(submodules_[static_cast<size_t>(row)].path);
    return {};
}

QString SubmoduleTableModel::urlAtRow(int row) const {
    if (row >= 0 && row < static_cast<int>(submodules_.size()))
        return QString::fromStdString(submodules_[static_cast<size_t>(row)].url);
    return {};
}

QString SubmoduleTableModel::statusString(git::SubmoduleStatus status) {
    switch (status) {
    case git::SubmoduleStatus::Clean:         return tr("Clean");
    case git::SubmoduleStatus::Dirty:         return tr("Dirty");
    case git::SubmoduleStatus::Uninitialized: return tr("Uninitialized");
    case git::SubmoduleStatus::OutOfDate:     return tr("Out of Date");
    case git::SubmoduleStatus::Added:         return tr("Added");
    case git::SubmoduleStatus::Deleted:       return tr("Deleted");
    case git::SubmoduleStatus::Modified:      return tr("Modified");
    }
    return tr("Unknown");
}

// ===========================================================================
// SubmoduleWidget
// ===========================================================================

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

SubmoduleWidget::SubmoduleWidget(QWidget* parent)
    : RecordTablePanel(parent)
    , model_(new SubmoduleTableModel(this))
{
    setupUI();
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

void SubmoduleWidget::setSubmodules(std::vector<git::SubmoduleInfo> submodules) {
    model_->setSubmodules(std::move(submodules));
    table_->resizeColumnsToContents();
}

void SubmoduleWidget::clear() {
    model_->clear();
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

QString SubmoduleWidget::selectedSubmoduleName() const {
    return model_->nameAtRow(selectedRow());
}

QString SubmoduleWidget::selectedSubmodulePath() const {
    return model_->pathAtRow(selectedRow());
}

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------

void SubmoduleWidget::onInitClicked() {
    const QString name = selectedSubmoduleName();
    if (!name.isEmpty())
        emit initRequested(name);
}

void SubmoduleWidget::onUpdateClicked() {
    const QString name = selectedSubmoduleName();
    if (!name.isEmpty())
        emit updateRequested(name);
}

void SubmoduleWidget::onSyncClicked() {
    const QString name = selectedSubmoduleName();
    if (!name.isEmpty())
        emit syncRequested(name);
}

void SubmoduleWidget::onDeinitClicked() {
    const QString name = selectedSubmoduleName();
    if (!name.isEmpty())
        emit deinitRequested(name);
}

void SubmoduleWidget::onContextMenu(const QPoint& pos) {
    const QModelIndex index = table_->indexAt(pos);
    if (!index.isValid())
        return;

    const int row = index.row();
    const QString name = model_->nameAtRow(row);
    const QString path = model_->pathAtRow(row);
    const QString url  = model_->urlAtRow(row);

    if (name.isEmpty())
        return;

    QMenu menu(this);

    menu.addAction(tr("Init"), this, [this, name]() {
        emit initRequested(name);
    });

    menu.addAction(tr("Update"), this, [this, name]() {
        emit updateRequested(name);
    });

    menu.addAction(tr("Deinit"), this, [this, name]() {
        emit deinitRequested(name);
    });

    menu.addSeparator();

    // GitBolt has one window: Open makes the submodule the repository
    // it shows, as Recent does.
    menu.addAction(tr("Open in GitBolt"), this, [this, path]() {
        emit openRequested(path);
    });
    menu.addAction(tr("Open in File Explorer"), this, [this, path]() {
        emit revealRequested(path);
    });

    menu.addAction(tr("Copy URL"), this, [url]() {
        QApplication::clipboard()->setText(url);
    });

    menu.exec(table_->viewport()->mapToGlobal(pos));
}

// ---------------------------------------------------------------------------
// UI setup
// ---------------------------------------------------------------------------

void SubmoduleWidget::setupUI() {
    // Toolbar actions (toolbar/table scaffold lives in
    // RecordTablePanel — shared with every record-table panel).

    initAction_ = toolbar_->addAction(
        QIcon::fromTheme(QStringLiteral("document-new")),
        tr("Init"));
    initAction_->setObjectName(QStringLiteral("submodule.init"));
    connect(initAction_, &QAction::triggered,
            this, &SubmoduleWidget::onInitClicked);

    updateAction_ = toolbar_->addAction(
        QIcon::fromTheme(QStringLiteral("view-refresh")),
        tr("Update"));
    updateAction_->setObjectName(QStringLiteral("submodule.update"));
    connect(updateAction_, &QAction::triggered,
            this, &SubmoduleWidget::onUpdateClicked);

    syncAction_ = toolbar_->addAction(
        QIcon::fromTheme(QStringLiteral("sync")),
        tr("Sync"));
    syncAction_->setObjectName(QStringLiteral("submodule.sync"));
    connect(syncAction_, &QAction::triggered,
            this, &SubmoduleWidget::onSyncClicked);

    deinitAction_ = toolbar_->addAction(
        QIcon::fromTheme(QStringLiteral("process-stop")),
        tr("Deinit"));
    deinitAction_->setObjectName(QStringLiteral("submodule.deinit"));
    connect(deinitAction_, &QAction::triggered,
            this, &SubmoduleWidget::onDeinitClicked);

    initPanel(model_);

    // Connections
    connect(table_, &QTableView::customContextMenuRequested,
            this, &SubmoduleWidget::onContextMenu);
}

} // namespace gitbolt::widgets
