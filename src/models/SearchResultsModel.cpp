#include "models/SearchResultsModel.h"

namespace gitbolt::models {

SearchResultsModel::SearchResultsModel(QObject* parent) : QAbstractTableModel(parent) {}

int SearchResultsModel::rowCount(const QModelIndex& /*parent*/) const { return 0; }
int SearchResultsModel::columnCount(const QModelIndex& /*parent*/) const { return 1; }

QVariant SearchResultsModel::data(const QModelIndex& /*index*/, int /*role*/) const { return {}; }

QVariant SearchResultsModel::headerData(int /*section*/, Qt::Orientation /*orientation*/, int /*role*/) const { return {}; }

void SearchResultsModel::clear() {}

} // namespace gitbolt::models
