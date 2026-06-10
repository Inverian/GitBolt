#include "widgets/LoadingOverlayWidget.h"

#include <QEvent>
#include <QPainter>
#include <QPen>
#include <QTimer>

namespace gitbolt::widgets {

namespace {
constexpr int kTickMs       = 30;  // ~33 fps
constexpr int kDegPerTick   = 8;   // full revolution ≈ 1.35 s
constexpr int kSpinnerSize  = 48;  // arc diameter, px
constexpr int kSpinnerSpan  = 270; // arc length, degrees
constexpr int kPenWidth     = 4;
constexpr int kTextGap      = 18;  // spinner-to-text spacing, px
} // namespace

LoadingOverlayWidget::LoadingOverlayWidget(QWidget* parent)
    : QWidget(parent)
{
    Q_ASSERT(parent);
    hide();

    timer_ = new QTimer(this);
    timer_->setInterval(kTickMs);
    connect(timer_, &QTimer::timeout, this, [this]() {
        angle_ = (angle_ + kDegPerTick) % 360;
        update();
    });

    // Track the parent's size so the veil always covers it edge to
    // edge, including splitter drags and window resizes while the
    // overlay is up.
    parent->installEventFilter(this);
}

void LoadingOverlayWidget::showOverlay(const QString& message)
{
    message_ = message;
    if (parentWidget())
        setGeometry(parentWidget()->rect());
    raise();
    show();
}

void LoadingOverlayWidget::hideOverlay()
{
    hide();
}

void LoadingOverlayWidget::setMessage(const QString& message)
{
    if (message_ == message)
        return;
    message_ = message;
    update();
}

void LoadingOverlayWidget::paintEvent(QPaintEvent* /*event*/)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    // Translucent veil — keeps the (empty) view faintly visible so
    // the transition doesn't flash, while making it obvious that
    // the content isn't ready yet.
    QColor veil = palette().color(QPalette::Window);
    veil.setAlpha(180);
    p.fillRect(rect(), veil);

    // Spinner: a 270° arc rotating around the widget centre,
    // slightly above centre so the spinner+text block reads as one
    // vertically-centred unit.
    const QPointF c(width() / 2.0, height() / 2.0 - kTextGap);
    const QRectF arcBox(c.x() - kSpinnerSize / 2.0,
                        c.y() - kSpinnerSize / 2.0,
                        kSpinnerSize, kSpinnerSize);

    QPen pen(palette().color(QPalette::Highlight), kPenWidth,
             Qt::SolidLine, Qt::RoundCap);
    p.setPen(pen);
    // drawArc takes 1/16th-degree units; negative span = clockwise.
    p.drawArc(arcBox, -angle_ * 16, -kSpinnerSpan * 16);

    if (!message_.isEmpty()) {
        p.setPen(palette().color(QPalette::WindowText));
        QRectF textRect(0, arcBox.bottom() + kTextGap,
                        width(), fontMetrics().height() * 2.0);
        p.drawText(textRect, Qt::AlignHCenter | Qt::AlignTop, message_);
    }
}

void LoadingOverlayWidget::showEvent(QShowEvent* event)
{
    timer_->start();
    QWidget::showEvent(event);
}

void LoadingOverlayWidget::hideEvent(QHideEvent* event)
{
    timer_->stop();
    QWidget::hideEvent(event);
}

bool LoadingOverlayWidget::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == parentWidget() && event->type() == QEvent::Resize
        && isVisible()) {
        setGeometry(parentWidget()->rect());
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace gitbolt::widgets
