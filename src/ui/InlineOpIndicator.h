#pragma once

#include <QObject>
#include <QString>

class QLabel;
class QTimer;

namespace gitbolt::ui {

/// Drives the inline toolbar status label that narrates one
/// operation at a time:
///
///     start("Fetching from origin…")     italic, persists
///     succeed("Fetch complete.")         green "✓ …", auto-clears
///     fail("fetch failed: …", full)      red bold "✗ …", tooltip
///                                        carries the full text,
///                                        longer auto-clear, or none
///                                        (clearAfterMs <= 0: stays
///                                        until the next start())
///     flash("Refreshed.")                plain transient note
///
/// Replaces three hand-rolled copies of the same label/stylesheet/
/// timer dance (toolbar remote ops, the Refresh flash, and the
/// branch-combo checkout feedback) that had drifted apart in
/// timeouts and styling.
class InlineOpIndicator : public QObject {
    Q_OBJECT
public:
    explicit InlineOpIndicator(QLabel* label, QObject* parent = nullptr);

    void start(const QString& msg);
    void succeed(const QString& msg, int clearAfterMs = 4000);
    void fail(const QString& oneLine, const QString& fullTooltip,
              int clearAfterMs = 8000);
    void flash(const QString& msg, int clearAfterMs = 1500);

    /// Stop a pending auto-clear (call when a new op begins so the
    /// previous op's timer can't blank the fresh message mid-run).
    void cancelPendingClear();

    /// Blank the label now, for an op dropped before it finished.
    void clear();

private:
    /// Set the label, elided to a bounded width; an elided message
    /// keeps its full text in the tooltip (or `tooltip` when given).
    void setText(const QString& text, const QString& tooltip = QString());
    void scheduleClear(int ms);
    void clearNow();

    QLabel* label_;
    QTimer* timer_ = nullptr;
};

} // namespace gitbolt::ui
