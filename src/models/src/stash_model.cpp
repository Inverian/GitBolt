#include "gitbolt/models/stash_model.h"

namespace gitbolt::models {

StashModel::StashModel(QObject* parent) : QAbstractTableModel(parent) {}

int StashModel::rowCount(const QModelIndex& /*parent*/) const { return 0; }
int StashModel::columnCount(const QModelIndex& /*parent*/) const { return 1; }

QVariant StashModel::data(const QModelIndex& /*index*/, int /*role*/) const { return {}; }

QVariant StashModel::headerData(int /*section*/, Qt::Orientation /*orientation*/, int /*role*/) const { return {}; }

void StashModel::clear() {}

} // namespace gitbolt::models
