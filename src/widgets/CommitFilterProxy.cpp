#include "widgets/CommitFilterProxy.h"
#include "models/CommitLogModel.h"

#include <QDateTime>

namespace gitbolt::widgets {

CommitFilterProxy::CommitFilterProxy(QObject* parent)
    : QSortFilterProxyModel(parent) {
    // The filter compares against the message column for the
    // base-class fallback, but our filterAcceptsRow override
    // handles the actual logic. Setting the key column and case
    // here keeps the toolbar's existing setFilterFixedString
    // semantics consistent if anything bypasses setMessageFilter.
    setFilterCaseSensitivity(Qt::CaseInsensitive);
    setFilterKeyColumn(
        static_cast<int>(models::CommitLogColumn::Message));
}

// begin/endFilterChange() replace the deprecated invalidateFilter()
// from Qt 6.10 on; older Qt (README minimum is 6.5) lacks
// endFilterChange(), so keep the old call there.
void CommitFilterProxy::setCriteria(const Criteria& c) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    beginFilterChange();
    criteria_ = c;
    endFilterChange();
#else
    criteria_ = c;
    invalidateFilter();
#endif
}

void CommitFilterProxy::setMessageFilter(const QString& text) {
    if (criteria_.messageContains == text) return;
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    beginFilterChange();
    criteria_.messageContains = text;
    endFilterChange();
#else
    criteria_.messageContains = text;
    invalidateFilter();
#endif
}

bool CommitFilterProxy::filterAcceptsRow(int srcRow,
                                         const QModelIndex& parent) const {
    if (!criteria_.isAnyActive()) return true;

    auto* model = qobject_cast<models::CommitLogModel*>(sourceModel());
    if (!model) {
        // Source isn't a CommitLogModel; fall back to base class
        // (handles any other proxy chain shape gracefully).
        return QSortFilterProxyModel::filterAcceptsRow(srcRow, parent);
    }
    const auto* commit = model->commitAt(srcRow);
    if (!commit) return false;

    // Message: substring match against the summary line. Author
    // gets to see their first-line subject the same way the table
    // displays it, so users don't get surprised that bodies match
    // when they only see the summary in the grid.
    if (!criteria_.messageContains.isEmpty()) {
        const QString summary = QString::fromStdString(commit->summary);
        if (!summary.contains(criteria_.messageContains,
                              Qt::CaseInsensitive))
            return false;
    }

    // Author: matches name OR email. Either field is enough so
    // users can search by the part they remember.
    if (!criteria_.authorContains.isEmpty()) {
        const QString name = QString::fromStdString(commit->author.name);
        const QString email = QString::fromStdString(commit->author.email);
        const bool inName = name.contains(criteria_.authorContains,
                                          Qt::CaseInsensitive);
        const bool inEmail = email.contains(criteria_.authorContains,
                                            Qt::CaseInsensitive);
        if (!inName && !inEmail) return false;
    }

    // SHA prefix: case-insensitive prefix on the full hex SHA.
    if (!criteria_.shaPrefix.isEmpty()) {
        const QString fullHex = QString::fromStdString(commit->id.toHex());
        if (!fullHex.startsWith(criteria_.shaPrefix,
                                Qt::CaseInsensitive))
            return false;
    }

    // Date range. CommitData::author.when is a chrono time_point;
    // convert to QDate for a half-open inclusive comparison
    // [fromDate, toDate]. Either bound is optional.
    if (criteria_.fromDate.isValid() || criteria_.toDate.isValid()) {
        const auto t = std::chrono::system_clock::to_time_t(
            commit->author.when);
        const QDate when = QDateTime::fromSecsSinceEpoch(
            static_cast<qint64>(t)).date();
        if (criteria_.fromDate.isValid() && when < criteria_.fromDate)
            return false;
        if (criteria_.toDate.isValid() && when > criteria_.toDate)
            return false;
    }

    return true;
}

} // namespace gitbolt::widgets
