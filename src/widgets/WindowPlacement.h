#pragma once

#include <QRect>

class QWidget;

namespace gitbolt::widgets {

/// `frame` (a window's frame geometry) fitted to `available` (its
/// screen's available geometry): no larger than it, then moved just far
/// enough to lie inside it. A frame that already fits comes back as is.
QRect fitWindowFrame(const QRect& frame, const QRect& available);

/// Shrinks and moves the top-level `window` into the available area of
/// the screen it is on (fitWindowFrame), if it isn't there already. A
/// maximized or full-screen window is the system's to fit.
void fitOnScreen(QWidget* window);

} // namespace gitbolt::widgets
