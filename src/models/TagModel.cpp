#include "models/TagModel.h"

namespace gitbolt::models {

TagModel::TagModel(QObject* parent) : QAbstractTableModel(parent) {}

int TagModel::rowCount(const QModelIndex& /*parent*/) const { return 0; }
int TagModel::columnCount(const QModelIndex& /*parent*/) const { return 1; }

QVariant TagModel::data(const QModelIndex& /*index*/, int /*role*/) const { return {}; }

QVariant TagModel::headerData(int /*section*/, Qt::Orientation /*orientation*/, int /*role*/) const { return {}; }

void TagModel::clear() {}

} // namespace gitbolt::models
