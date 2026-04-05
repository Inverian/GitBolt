#pragma once

#include "git/Repository.h"

#include <QAbstractTableModel>
#include <QAction>
#include <QTableView>
#include <QToolBar>
#include <QWidget>
#include <vector>

namespace gitbolt::widgets {

// ---------------------------------------------------------------------------
// Internal table model for submodule data
// ---------------------------------------------------------------------------

class SubmoduleTableModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column {
        ColName = 0,
        ColPath,
        ColURL,
        ColStatus,
        ColumnCount,
    };

    enum Roles {
        SubmoduleNameRole = Qt::UserRole + 1,
        SubmodulePathRole,
        SubmoduleURLRole,
        SubmoduleStatusRole,
    };

    explicit SubmoduleTableModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

    void setSubmodules(std::vector<gitbolt::git::SubmoduleInfo> submodules);
    void clear();

    QString nameAtRow(int row) const;
    QString pathAtRow(int row) const;
    QString urlAtRow(int row) const;

private:
    static QString statusString(gitbolt::git::SubmoduleStatus status);

    std::vector<gitbolt::git::SubmoduleInfo> submodules_;
};

// ---------------------------------------------------------------------------
// SubmoduleWidget
// ---------------------------------------------------------------------------

class SubmoduleWidget : public QWidget {
    Q_OBJECT
public:
    explicit SubmoduleWidget(QWidget* parent = nullptr);

    void setSubmodules(std::vector<gitbolt::git::SubmoduleInfo> submodules);
    void clear();

signals:
    void initRequested(const QString& name);
    void updateRequested(const QString& name);
    void deinitRequested(const QString& name);
    void syncRequested(const QString& name);
    void openRequested(const QString& path);

private slots:
    void onContextMenu(const QPoint& pos);
    void onInitClicked();
    void onUpdateClicked();
    void onSyncClicked();
    void onDeinitClicked();

private:
    void setupUI();
    QString selectedSubmoduleName() const;
    QString selectedSubmodulePath() const;

    QTableView* tableView_ = nullptr;
    SubmoduleTableModel* model_ = nullptr;
    QToolBar* toolbar_ = nullptr;
    QAction* initAction_ = nullptr;
    QAction* updateAction_ = nullptr;
    QAction* syncAction_ = nullptr;
    QAction* deinitAction_ = nullptr;
};

} // namespace gitbolt::widgets
