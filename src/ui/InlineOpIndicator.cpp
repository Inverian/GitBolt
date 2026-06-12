#include "ui/InlineOpIndicator.h"

#include <QLabel>
#include <QTimer>

namespace gitbolt::ui {

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
    label_->setText(msg);
    label_->setToolTip(QString());
}

void InlineOpIndicator::succeed(const QString& msg, int clearAfterMs)
{
    if (label_) {
        label_->setStyleSheet(QStringLiteral(
            "QLabel { color: #2e9c36; font-weight: bold; }"));
        label_->setText(QStringLiteral("✓ %1").arg(msg));
        label_->setToolTip(QString());
    }
    scheduleClear(clearAfterMs);
}

void InlineOpIndicator::fail(const QString& oneLine,
                             const QString& fullTooltip, int clearAfterMs)
{
    if (label_) {
        label_->setStyleSheet(QStringLiteral(
            "QLabel { color: #c43c3c; font-weight: bold; }"));
        label_->setText(QStringLiteral("✗ %1").arg(oneLine));
        label_->setToolTip(fullTooltip);
    }
    scheduleClear(clearAfterMs);
}

void InlineOpIndicator::flash(const QString& msg, int clearAfterMs)
{
    if (label_) {
        label_->setStyleSheet(QString());
        label_->setText(msg);
        label_->setToolTip(QString());
    }
    scheduleClear(clearAfterMs);
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
