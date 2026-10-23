#include "ui/InlineOpIndicator.h"

#include "util/ToolTip.h"

#include <QApplication>
#include <QEvent>
#include <QPalette>

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
    if (label_) {
        label_->installEventFilter(this);
        // What it says is mostly git's: a failure's first line of
        // stderr, which a server can write ("remote: …"), or a branch
        // name. A QLabel renders a first line with a tag in it as rich
        // text, so a server's "<font color=…>" coloured a failure
        // green. (Plain, the eliding in setText() is exact too: it cut
        // through the markup, and a cut tag showed or vanished.)
        label_->setTextFormat(Qt::PlainText);
    }
}

void InlineOpIndicator::start(const QString& msg)
{
    cancelPendingClear();
    outcome_ = Outcome::None;
    if (!label_)
        return;
    label_->setStyleSheet(QStringLiteral(
        "QLabel { color: palette(window-text); font-style: italic; }"));
    setText(msg);
}

namespace {
// The theme's green or red for an outcome: the light ones read at 2.7:1
// to 3.9:1 on the dark theme's toolbar. Each reads at 4.5:1 or more on
// its own theme's (the light green, #2e9c36, was 3.1:1 there).
QString outcomeColor(bool success)
{
    const QPalette app = QApplication::palette();
    const bool dark = app.color(QPalette::WindowText).lightness()
                      > app.color(QPalette::Window).lightness();
    if (success)
        return dark ? QStringLiteral("#3fb950") : QStringLiteral("#1e7b34");
    // #c03a3a, not #c43c3c: that one read at 4.4997:1 on the light
    // tool bar (#efefef), a shade under 4.5.
    return dark ? QStringLiteral("#ff7b72") : QStringLiteral("#c03a3a");
}
} // namespace

void InlineOpIndicator::applyOutcomeStyle()
{
    if (!label_ || outcome_ == Outcome::None)
        return;
    const QString sheet = QStringLiteral("QLabel { color: %1; font-weight: bold; }")
                              .arg(outcomeColor(outcome_ == Outcome::Success));
    // Setting it changes the label's palette, which comes back here:
    // only a sheet that differs is set.
    if (label_->styleSheet() != sheet)
        label_->setStyleSheet(sheet);
}

bool InlineOpIndicator::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == label_ && outcome_ != Outcome::None
        && (event->type() == QEvent::ApplicationPaletteChange
            || event->type() == QEvent::PaletteChange))
        applyOutcomeStyle();
    return QObject::eventFilter(watched, event);
}

void InlineOpIndicator::succeed(const QString& msg, int clearAfterMs)
{
    outcome_ = Outcome::Success;
    if (label_) {
        applyOutcomeStyle();
        setText(QStringLiteral("✓ %1").arg(msg));
    }
    scheduleClear(clearAfterMs);
}

void InlineOpIndicator::fail(const QString& oneLine,
                             const QString& fullTooltip, int clearAfterMs)
{
    outcome_ = Outcome::Failure;
    if (label_) {
        applyOutcomeStyle();
        setText(QStringLiteral("✗ %1").arg(oneLine), fullTooltip);
    }
    if (clearAfterMs > 0)
        scheduleClear(clearAfterMs);
    else
        cancelPendingClear();
}

void InlineOpIndicator::flash(const QString& msg, int clearAfterMs)
{
    outcome_ = Outcome::None;
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
    // The tooltip is git's whole message: plain text too.
    label_->setToolTip(util::plainToolTip(!tooltip.isEmpty() ? tooltip
                                          : shown != text   ? text
                                                            : QString()));
}

void InlineOpIndicator::cancelPendingClear()
{
    if (timer_ && timer_->isActive())
        timer_->stop();
}

void InlineOpIndicator::clear()
{
    cancelPendingClear();
    clearNow();
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
    outcome_ = Outcome::None;
    if (label_) {
        label_->clear();
        label_->setStyleSheet(QString());
        label_->setToolTip(QString());
    }
}

} // namespace gitbolt::ui
