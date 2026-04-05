#pragma once
#include <string>
#include <QAbstractTableModel>
#include <vector>

namespace gitbolt::models {

class ReflogModel : public QAbstractTableModel {
    Q_OBJECT
public:
    explicit ReflogModel(QObject* parent = nullptr);
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    void clear();
    // ReflogEntry defined locally
};

} // namespace gitbolt::models
