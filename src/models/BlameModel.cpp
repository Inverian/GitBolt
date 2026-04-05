#include "models/BlameModel.h"

namespace gitbolt::models {

BlameModel::BlameModel(QObject* parent) : QAbstractTableModel(parent) {}

int BlameModel::rowCount(const QModelIndex& /*parent*/) const { return 0; }
int BlameModel::columnCount(const QModelIndex& /*parent*/) const { return 1; }

QVariant BlameModel::data(const QModelIndex& /*index*/, int /*role*/) const { return {}; }

QVariant BlameModel::headerData(int /*section*/, Qt::Orientation /*orientation*/, int /*role*/) const { return {}; }

void BlameModel::clear() {}

} // namespace gitbolt::models
