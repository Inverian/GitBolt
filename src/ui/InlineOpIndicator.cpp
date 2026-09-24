#include "ui/InlineOpIndicator.h"

#include <QFontMetrics>
#include <QLabel>
#include <QMargins>
#include <QTimer>

namespace gitbolt::ui {

namespace {
// Widest the label may grow. The toolbar moves widgets that don't fit
// into its overflow menu, so an unbounded label (a failure's one-line
// stderr easily runs to a few hundred characters) pushed the Filter box
// off the toolbar, and with failures now sticky it would stay hidden.
constexpr int kMaxTextWidth = 360;
} // namespace

InlineOpIndicator::InlineOpIndicator(QLabel* label, QObject* parent)
    : QObject(parent), label_(label)
{
}

void InlineOpIndicator::start(const QString& msg)
{
    cancelPendingClear();
    if (!label_)
        return;
    label_->setStyleSheet(QStringLiteral(
        "QLabel { color: palette(window-text); font-style: italic; }"));
    setText(msg);
}

void InlineOpIndicator::succeed(const QString& msg, int clearAfterMs)
{
    if (label_) {
        label_->setStyleSheet(QStringLiteral(
            "QLabel { color: #2e9c36; font-weight: bold; }"));
        setText(QStringLiteral("✓ %1").arg(msg));
    }
    scheduleClear(clearAfterMs);
}

void InlineOpIndicator::fail(const QString& oneLine,
                             const QString& fullTooltip, int clearAfterMs)
{
    if (label_) {
        label_->setStyleSheet(QStringLiteral(
            "QLabel { color: #c43c3c; font-weight: bold; }"));
        setText(QStringLiteral("✗ %1").arg(oneLine), fullTooltip);
    }
    if (clearAfterMs > 0)
        scheduleClear(clearAfterMs);
    else
        cancelPendingClear();
}

void InlineOpIndicator::flash(const QString& msg, int clearAfterMs)
{
    if (label_) {
        label_->setStyleSheet(QString());
        setText(msg);
    }
    scheduleClear(clearAfterMs);
}

void InlineOpIndicator::setText(const QString& text, const QString& tooltip)
{
    const QMargins m = label_->contentsMargins();
    label_->setMaximumWidth(kMaxTextWidth + m.left() + m.right());
    // Measured after the caller's setStyleSheet, so a bold status is
    // elided with bold metrics.
    const QString shown = label_->fontMetrics().elidedText(
        text, Qt::ElideRight, kMaxTextWidth);
    label_->setText(shown);
    label_->setToolTip(!tooltip.isEmpty() ? tooltip
                       : shown != text   ? text
                                         : QString());
}

void InlineOpIndicator::cancelPendingClear()
{
    if (timer_ && timer_->isActive())
        timer_->stop();
}

void InlineOpIndicator::scheduleClear(int ms)
{
    if (!timer_) {
        timer_ = new QTimer(this);
        timer_->setSingleShot(true);
        connect(timer_, &QTimer::timeout,
                this, &InlineOpIndicator::clearNow);
    }
    timer_->start(ms);
}

void InlineOpIndicator::clearNow()
{
    if (label_) {
        label_->clear();
        label_->setStyleSheet(QString());
        label_->setToolTip(QString());
    }
}

} // namespace gitbolt::ui
