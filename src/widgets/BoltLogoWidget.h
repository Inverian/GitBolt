#pragma once

#include <QPixmap>
#include <QRegion>
#include <QVector>
#include <QWidget>

class QTimer;

namespace gitbolt::widgets {

/// Decorative widget that renders GitBolt's lightning-bolt logo in
/// the empty bottom-right corner of the dashboard. The bolt shape
/// is loaded from resources/icons/gitbolt-128.png (the same logo
/// the rest of the app uses) so the silhouette is a pixel-perfect
/// match to the real artwork.
///
/// Two post-processing passes on load:
///   1. Dark background pixels → transparent, so no rounded-square
///      backing plate appears under the widget.
///   2. Remaining opaque pixels → flattened to the bolt's yellow,
///      wiping the baked-in binary digits. We draw our own
///      animated grid of 0/1s on top so they can flip live.
///
/// The overlaid digits flicker asynchronously (a small random
/// subset flips bits on every timer tick), clipped to the bolt
/// via SourceAtop composition. A subtle diagonal shimmer band
/// also drifts across to add movement without flashing colors.
class BoltLogoWidget : public QWidget {
    Q_OBJECT
public:
    explicit BoltLogoWidget(QWidget* parent = nullptr);

    // Digit grid constants — public so the .cpp's file-scope sprite
    // table can size itself against them.
    static constexpr int kDigitCols = 6;
    static constexpr int kDigitRows = 10;

protected:
    void paintEvent(QPaintEvent* event) override;

    /// Pause the animation timer when the widget isn't on screen.
    /// Saves the ~12 paint cycles / second when the user is in
    /// the repository view (dashboard hidden behind QStackedWidget)
    /// or after the window is minimized.
    void hideEvent(QHideEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    void prepareLogoPixmap();
    void stepAnimation();

    // The PNG with dark-background pixels turned transparent and
    // the yellow body flattened (digits wiped), rescaled once to
    // the widget's fixed size.
    QPixmap logoPixmap_;


    // Two clip regions, both computed once at construction:
    //   - boltRegion_:     every opaque pixel (border + interior).
    //                      Used for the shimmer pass so the
    //                      highlight crosses the whole silhouette.
    //   - interiorRegion_: opaque pixels that are NOT part of the
    //                      border ring. Used for the digit overlay
    //                      so 0s/1s appear only on the lighter body
    //                      and never on top of the dark outline,
    //                      matching how the real logo has digits
    //                      only inside the bolt's fill area.
    QRegion boltRegion_;
    QRegion interiorRegion_;

    // Grid of 0/1 values overlaid on the bolt. Flipped randomly
    // on each timer tick to look like data scrolling through the
    // bolt. Stored as a flat vector indexed by (row*cols + col).
    QVector<int> digits_;

    // Shimmer animation phase: 0..1, advances each tick. Drives
    // the diagonal position of the brightness band in paintEvent.
    double shimmerPhase_ = 0.0;
    QTimer* timer_ = nullptr;
};

} // namespace gitbolt::widgets
