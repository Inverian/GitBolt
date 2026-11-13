#pragma once

#include <QRect>

class QWidget;

namespace gitbolt::conf {

/// `frame` (a window's frame geometry) fitted to `available` (its
/// screen's available geometry): no larger than it, then moved just far
/// enough to lie inside it. A frame that already fits comes back as is.
QRect fitWindowFrame(const QRect& frame, const QRect& available);

/// Fits the top-level `window` into its screen's available area, for
/// calling as it shows. A placed window (a restored geometry, or one on
/// screen) moves just far enough; one still to be placed by the system
/// is left to it unless it's too large, when it's made to fit, centred
/// on its parent or the screen. Maximized or full screen: the system's.
void fitOnScreen(QWidget* window);

/// fitOnScreen() at every show of `window`, before its own showEvent
/// (where QDialog centres a dialog over its parent).
void fitOnScreenWhenShown(QWidget* window);

} // namespace gitbolt::conf
