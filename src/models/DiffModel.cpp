#include "models/DiffModel.h"

namespace gitbolt::models {

DiffModel::DiffModel(QObject* parent) : QAbstractTableModel(parent) {}

int DiffModel::rowCount(const QModelIndex& /*parent*/) const { return 0; }
int DiffModel::columnCount(const QModelIndex& /*parent*/) const { return 1; }

QVariant DiffModel::data(const QModelIndex& /*index*/, int /*role*/) const { return {}; }

QVariant DiffModel::headerData(int /*section*/, Qt::Orientation /*orientation*/, int /*role*/) const { return {}; }

void DiffModel::clear() {}

} // namespace gitbolt::models
