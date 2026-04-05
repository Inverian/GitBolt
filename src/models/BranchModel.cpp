#include "models/BranchModel.h"

namespace gitbolt::models {

BranchModel::BranchModel(QObject* parent) : QAbstractItemModel(parent) {}

int BranchModel::rowCount(const QModelIndex& /*parent*/) const { return 0; }
int BranchModel::columnCount(const QModelIndex& /*parent*/) const { return 1; }

QVariant BranchModel::data(const QModelIndex& /*index*/, int /*role*/) const { return {}; }

QVariant BranchModel::headerData(int /*section*/, Qt::Orientation /*orientation*/, int /*role*/) const { return {}; }

void BranchModel::clear() {}

} // namespace gitbolt::models
