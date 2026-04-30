#pragma once

#include <QDate>
#include <QSortFilterProxyModel>
#include <QString>

namespace gitbolt::widgets {

/// Custom proxy that knows about CommitLogModel and lets the user
/// filter the visible rows by multiple commit attributes at once.
/// Replaces the plain QSortFilterProxyModel that previously sat
/// between CommitLogModel and the table view in
/// RevisionGraphWidget.
///
/// All criteria combine with AND. Empty / invalid criteria are
/// ignored, so by default (everything blank) the proxy passes
/// every row through. The toolbar Filter input continues to work
/// by setting `messageContains` only.
class CommitFilterProxy : public QSortFilterProxyModel {
    Q_OBJECT
public:
    struct Criteria {
        QString messageContains;   ///< case-insensitive substring
                                   ///  match against commit summary
        QString authorContains;    ///< matches author name or email
        QString shaPrefix;         ///< matches start of full SHA
        QDate   fromDate;          ///< invalid = no lower bound
        QDate   toDate;            ///< invalid = no upper bound

        bool isAnyActive() const {
            return !messageContains.isEmpty() ||
                   !authorContains.isEmpty() ||
                   !shaPrefix.isEmpty() ||
                   fromDate.isValid() || toDate.isValid();
        }
    };

    explicit CommitFilterProxy(QObject* parent = nullptr);

    /// Replace all criteria at once and re-run the filter. Used
    /// by AdvancedFilterDialog.
    void setCriteria(const Criteria& c);
    Criteria criteria() const { return criteria_; }

    /// Convenience for the toolbar Filter input: sets just the
    /// message-contains field, leaves the rest untouched. Allows
    /// the user to type a quick filter without the advanced
    /// dialog clobbering an existing date / author criterion.
    void setMessageFilter(const QString& text);

protected:
    bool filterAcceptsRow(int srcRow,
                          const QModelIndex& parent) const override;

private:
    Criteria criteria_;
};

} // namespace gitbolt::widgets
