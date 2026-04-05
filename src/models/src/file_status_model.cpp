#include "gitbolt/models/file_status_model.h"

namespace gitbolt::models {

FileStatusModel::FileStatusModel(QObject* parent) : QAbstractTableModel(parent) {}

int FileStatusModel::rowCount(const QModelIndex& /*parent*/) const { return 0; }
int FileStatusModel::columnCount(const QModelIndex& /*parent*/) const { return 1; }

QVariant FileStatusModel::data(const QModelIndex& /*index*/, int /*role*/) const { return {}; }

QVariant FileStatusModel::headerData(int /*section*/, Qt::Orientation /*orientation*/, int /*role*/) const { return {}; }

void FileStatusModel::clear() {}

} // namespace gitbolt::models
