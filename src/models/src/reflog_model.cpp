#include "gitbolt/models/reflog_model.h"

namespace gitbolt::models {

ReflogModel::ReflogModel(QObject* parent) : QAbstractTableModel(parent) {}

int ReflogModel::rowCount(const QModelIndex& /*parent*/) const { return 0; }
int ReflogModel::columnCount(const QModelIndex& /*parent*/) const { return 1; }

QVariant ReflogModel::data(const QModelIndex& /*index*/, int /*role*/) const { return {}; }

QVariant ReflogModel::headerData(int /*section*/, Qt::Orientation /*orientation*/, int /*role*/) const { return {}; }

void ReflogModel::clear() {}

} // namespace gitbolt::models
