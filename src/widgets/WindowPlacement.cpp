#include "widgets/WindowPlacement.h"

#include <QGuiApplication>
#include <QScreen>
#include <QWidget>

namespace gitbolt::widgets {

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

// The screen under the frame's centre, else the window's own. The
// frame's margins (title bar, borders) are kept: geometry() is inside.
void fitOnScreen(QWidget* window)
{
    if (!window || window->windowState() & (Qt::WindowMaximized | Qt::WindowFullScreen))
        return;
    const QRect frame = window->frameGeometry();
    const QScreen* screen = QGuiApplication::screenAt(frame.center());
    if (!screen)
        screen = window->screen();
    if (!screen)
        return;
    const QRect fitted = fitWindowFrame(frame, screen->availableGeometry());
    if (fitted == frame)
        return;
    const QRect inner = window->geometry();
    window->setGeometry(fitted.marginsRemoved(QMargins(inner.left() - frame.left(),
                                                       inner.top() - frame.top(),
                                                       frame.right() - inner.right(),
                                                       frame.bottom() - inner.bottom())));
}

} // namespace gitbolt::widgets
