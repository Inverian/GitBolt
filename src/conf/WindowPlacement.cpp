#include "conf/WindowPlacement.h"

#include <QEvent>
#include <QGuiApplication>
#include <QScreen>
#include <QWidget>
#include <QWindow>

namespace gitbolt::conf {

QRect fitWindowFrame(const QRect& frame, const QRect& available)
{
    if (!available.isValid())
        return frame;
    QRect fitted(frame.topLeft(), frame.size().boundedTo(available.size()));
    if (fitted.right() > available.right())
        fitted.moveRight(available.right());
    if (fitted.bottom() > available.bottom())
        fitted.moveBottom(available.bottom());
    if (fitted.left() < available.left())
        fitted.moveLeft(available.left());
    if (fitted.top() < available.top())
        fitted.moveTop(available.top());
    return fitted;
}

// A window neither moved (WA_Moved) nor on screen yet has a stale position
// (macOS: (0,0) until the system places it), so only its size counts and
// one that fits is left where the system puts it. geometry() is inside
// the frame's margins.
void fitOnScreen(QWidget* window)
{
    if (!window || window->windowState() & (Qt::WindowMaximized | Qt::WindowFullScreen))
        return;
    const QRect frame = window->frameGeometry();
    const QWindow* handle = window->windowHandle();
    const bool placed = window->testAttribute(Qt::WA_Moved) || (handle && handle->isVisible());
    const QScreen* screen = placed ? QGuiApplication::screenAt(frame.center()) : nullptr;
    if (!screen)
        screen = window->screen();
    if (!screen)
        return;
    const QRect available = screen->availableGeometry();
    QRect fitted = frame;
    if (placed) {
        fitted = fitWindowFrame(frame, available);
    } else if (frame.width() > available.width() || frame.height() > available.height()) {
        fitted.setSize(frame.size().boundedTo(available.size()));
        const QWidget* parent = window->parentWidget();
        fitted.moveCenter(parent ? parent->window()->frameGeometry().center() : available.center());
        fitted = fitWindowFrame(fitted, available);
    }
    if (fitted == frame)
        return;
    const QRect inner = window->geometry();
    window->setGeometry(fitted.marginsRemoved(QMargins(inner.left() - frame.left(),
                                                       inner.top() - frame.top(),
                                                       frame.right() - inner.right(),
                                                       frame.bottom() - inner.bottom())));
}

namespace {

// Event filters run before the widget's own showEvent.
class FitOnShow : public QObject {
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event->type() == QEvent::Show && !event->spontaneous())
            fitOnScreen(static_cast<QWidget*>(watched));
        return false;
    }
};

} // namespace

void fitOnScreenWhenShown(QWidget* window)
{
    if (window)
        window->installEventFilter(new FitOnShow(window));
}

} // namespace gitbolt::conf
