#include "widgets/BoltLogoWidget.h"

#include <QFont>
#include <QImage>
#include <QLinearGradient>
#include <QPainter>
#include <QRandomGenerator>
#include <QTimer>

namespace gitbolt::widgets {

namespace {

// Outline color and digit color sampled from the real logo
// (bright amber yellow). The body interior is TRANSPARENT rather
// than the PNG's original dark amber or the later solid black —
// the dashboard background shows through, so the bolt reads as
// an outlined stencil rather than a filled sticker.
const QColor kBoltOutline   (250, 181, 28);        // bright yellow outline
const QColor kDigitInk      (250, 181, 28, 240);   // same yellow as outline
// Shimmer colors are generic, not derived from the logo.
const QColor kShimmerEdge   (255, 255, 255, 0);    // transparent ends
const QColor kShimmerPeak   (255, 255, 220, 90);   // soft cream peak

// Border thickness in pixels at the final widget size. 2 gives a
// visible outline without eating too much into the interior on a
// ~42x60 widget.
constexpr int kBorderPx = 2;

} // namespace

BoltLogoWidget::BoltLogoWidget(QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_TranslucentBackground);

    // prepareLogoPixmap() crops the source to the bolt's tight
    // bounding box and sets the widget's fixed size to match that
    // aspect ratio. See the comments there for why we do this
    // before any other init — it's what lets equal positioning
    // margins in DashboardView translate to equal visual margins.
    prepareLogoPixmap();

    // Seed the digit grid with random bits so the first paint
    // doesn't show all zeros.
    digits_.resize(kDigitCols * kDigitRows);
    for (int i = 0; i < digits_.size(); ++i)
        digits_[i] = QRandomGenerator::global()->bounded(2);

    timer_ = new QTimer(this);
    // 80 ms per tick. Fast enough that the digits visibly stream
    // through the bolt but not so fast that individual flips blur
    // into each other. Combined with 4 cells flipping per tick
    // (see stepAnimation) this gives a busy "data flowing" look
    // without tipping into a jittery buzz.
    timer_->setInterval(80);
    connect(timer_, &QTimer::timeout, this, &BoltLogoWidget::stepAnimation);
    timer_->start();
}

void BoltLogoWidget::prepareLogoPixmap()
{
    QImage src(QStringLiteral(":/icons/gitbolt-128.png"));
    if (src.isNull()) {
        setFixedSize(1, 1);
        return;
    }

    src = src.convertToFormat(QImage::Format_ARGB32);

    // First pass: dark-background pixels → transparent, track
    // bounding box of what stays opaque so we can crop tight.
    // Colors aren't assigned yet — that happens post-scale in the
    // border/interior classification loop, because a 2-pixel
    // border measured at 256 px would become fractional at the
    // final ~42×60 widget size.
    const int w = src.width();
    const int h = src.height();
    int minX = w, minY = h, maxX = -1, maxY = -1;
    for (int y = 0; y < h; ++y) {
        auto* row = reinterpret_cast<QRgb*>(src.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const QRgb pixel = row[x];
            const int r = qRed(pixel);
            const int g = qGreen(pixel);
            const int b = qBlue(pixel);
            if (r + g + b < 210) {
                row[x] = qRgba(0, 0, 0, 0);
            } else {
                row[x] = qRgba(255, 255, 255, 255);  // mark opaque
                if (x < minX) minX = x;
                if (x > maxX) maxX = x;
                if (y < minY) minY = y;
                if (y > maxY) maxY = y;
            }
        }
    }

    // Crop to the bolt's tight bounding box.
    if (maxX >= minX && maxY >= minY) {
        src = src.copy(minX, minY,
                       maxX - minX + 1, maxY - minY + 1);
    }

    constexpr int kTargetHeight = 60;
    const qreal aspect = (src.height() > 0)
        ? static_cast<qreal>(src.width()) / src.height()
        : 1.0;
    const int targetW = qMax(1, qRound(kTargetHeight * aspect));
    setFixedSize(targetW, kTargetHeight);

    // Scale the processed image to the widget's target size — we
    // need widget-space coordinates for both the clip region and
    // the border/interior classification below.
    QImage scaled = src.scaled(size(), Qt::KeepAspectRatio,
                               Qt::SmoothTransformation)
                      .convertToFormat(QImage::Format_ARGB32);

    // Classify each opaque pixel as "border" (within kBorderPx of
    // a transparent pixel, i.e. near the silhouette edge) or
    // "interior" (fully surrounded by opaque pixels), painting
    // each class with the role color sampled above.
    //
    // We also accumulate a separate interior-only run tracker so
    // the digit overlay can clip to just the lighter body — the
    // user doesn't want digits showing on top of the dark border
    // ring, which would read as noise against the outline.
    // Interior "color" is fully transparent — the dashboard
    // background shows through the middle of the bolt, giving
    // the outline-only stencil look. Border is the sampled
    // yellow from the real logo.
    const QRgb fillOpaque   = qRgba(0, 0, 0, 0);
    const QRgb borderOpaque = qRgba(kBoltOutline.red(),
                                    kBoltOutline.green(),
                                    kBoltOutline.blue(), 255);

    auto isOpaqueAt = [&scaled](int x, int y) -> bool {
        if (x < 0 || y < 0 || x >= scaled.width() || y >= scaled.height())
            return false;  // treat out-of-bounds as transparent
        return qAlpha(scaled.pixel(x, y)) > 100;
    };

    // Work on a second image so we don't disturb alpha values
    // we're still reading from to classify nearby pixels.
    QImage colored = scaled;
    // Lay out the interior region during the same pass —
    // accumulating run rects row-by-row as we classify.
    interiorRegion_ = QRegion{};
    for (int y = 0; y < scaled.height(); ++y) {
        auto* row = reinterpret_cast<QRgb*>(colored.scanLine(y));
        int interiorRunStart = -1;
        for (int x = 0; x < scaled.width(); ++x) {
            if (!isOpaqueAt(x, y)) {
                if (interiorRunStart >= 0) {
                    interiorRegion_ += QRect(interiorRunStart, y,
                                             x - interiorRunStart, 1);
                    interiorRunStart = -1;
                }
                row[x] = qRgba(0, 0, 0, 0);
                continue;
            }
            bool nearEdge = false;
            for (int dy = -kBorderPx; dy <= kBorderPx && !nearEdge; ++dy) {
                for (int dx = -kBorderPx; dx <= kBorderPx && !nearEdge; ++dx) {
                    if (!isOpaqueAt(x + dx, y + dy))
                        nearEdge = true;
                }
            }
            row[x] = nearEdge ? borderOpaque : fillOpaque;
            if (nearEdge) {
                // Close out any open interior run; border pixels
                // are excluded from the digit clip region.
                if (interiorRunStart >= 0) {
                    interiorRegion_ += QRect(interiorRunStart, y,
                                             x - interiorRunStart, 1);
                    interiorRunStart = -1;
                }
            } else if (interiorRunStart < 0) {
                interiorRunStart = x;
            }
        }
        if (interiorRunStart >= 0) {
            interiorRegion_ += QRect(interiorRunStart, y,
                                     scaled.width() - interiorRunStart, 1);
        }
    }

    logoPixmap_ = QPixmap::fromImage(colored);

    // Build a QRegion covering every "sufficiently opaque" pixel
    // so the digit overlay and shimmer pass can clip to the bolt
    // silhouette at paint time. Alpha threshold 100 picks up edge
    // AA pixels while excluding the barely-there halo fringes
    // that would let digits leak slightly outside the silhouette.
    boltRegion_ = QRegion{};
    for (int y = 0; y < colored.height(); ++y) {
        int runStart = -1;
        for (int x = 0; x <= colored.width(); ++x) {
            const bool opaque = (x < colored.width())
                && qAlpha(colored.pixel(x, y)) > 100;
            if (opaque && runStart < 0) {
                runStart = x;
            } else if (!opaque && runStart >= 0) {
                boltRegion_ += QRect(runStart, y, x - runStart, 1);
                runStart = -1;
            }
        }
    }
}

void BoltLogoWidget::stepAnimation()
{
    // Flip a random subset of digits per tick. 4 flips on a ~60
    // cell grid at 80 ms per tick works out to ~50 flips/sec,
    // which reads as a lively binary stream without every cell
    // changing at once (that would just look like visual noise).
    // The values that DON'T flip on a given tick stay visible at
    // their current bit — that persistence is what makes the
    // overlay feel like real data rather than confetti.
    auto* rng = QRandomGenerator::global();
    const int flips = 4;
    const int total = digits_.size();
    for (int i = 0; i < flips && total > 0; ++i) {
        const int idx = rng->bounded(total);
        digits_[idx] ^= 1;
    }

    shimmerPhase_ += 0.025;
    if (shimmerPhase_ > 1.0)
        shimmerPhase_ -= 1.0;

    update();
}

void BoltLogoWidget::hideEvent(QHideEvent* event)
{
    // Pause the timer when the widget is off-screen — the dashboard
    // is in a QStackedWidget, so opening a repository hides it
    // entirely. No point burning ~12 frames/sec computing animation
    // for a widget no one can see.
    if (timer_ && timer_->isActive())
        timer_->stop();
    QWidget::hideEvent(event);
}

void BoltLogoWidget::showEvent(QShowEvent* event)
{
    if (timer_ && !timer_->isActive())
        timer_->start();
    QWidget::showEvent(event);
}

void BoltLogoWidget::paintEvent(QPaintEvent* /*event*/)
{
    if (logoPixmap_.isNull())
        return;

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);

    // --- Bolt base (border + fill, sampled from PNG) --------------
    const int originX = (width()  - logoPixmap_.width())  / 2;
    const int originY = (height() - logoPixmap_.height()) / 2;
    p.drawPixmap(originX, originY, logoPixmap_);

    // --- Animated digit grid (interior only) ----------------------
    //
    // Clipped to interiorRegion_ (NOT boltRegion_) so 0s/1s only
    // appear on the body fill, never spilling onto the darker
    // outline. Matches the real logo where the baked digits sit
    // strictly inside the bolt's interior.
    p.save();
    p.setClipRegion(interiorRegion_.translated(originX, originY));

    QFont digitFont(QStringLiteral("Menlo"));
    digitFont.setPixelSize(7);
    digitFont.setBold(true);
    p.setFont(digitFont);
    p.setPen(kDigitInk);

    const qreal cellW = static_cast<qreal>(width())  / kDigitCols;
    const qreal cellH = static_cast<qreal>(height()) / kDigitRows;
    for (int row = 0; row < kDigitRows; ++row) {
        for (int col = 0; col < kDigitCols; ++col) {
            const QRectF cell(col * cellW, row * cellH, cellW, cellH);
            const int bit = digits_[row * kDigitCols + col];
            p.drawText(cell, Qt::AlignCenter,
                       bit ? QStringLiteral("1") : QStringLiteral("0"));
        }
    }
    p.restore();

    // --- Shimmer pass (whole bolt, border + interior) -------------
    // Uses boltRegion_ so the highlight can travel across the
    // outline too — a shimmer that skipped the border would look
    // oddly chopped on the bolt's sharper angles.
    p.save();
    p.setClipRegion(boltRegion_.translated(originX, originY));
    const double phase = shimmerPhase_;
    const qreal wF = width();
    const qreal hF = height();
    const qreal cx = phase * wF * 1.8 - wF * 0.4;
    const qreal cy = phase * hF * 1.8 - hF * 0.4;
    const qreal bandHalfWidth = wF * 0.25;

    QLinearGradient grad(cx - bandHalfWidth, cy - bandHalfWidth,
                         cx + bandHalfWidth, cy + bandHalfWidth);
    grad.setColorAt(0.0, kShimmerEdge);
    grad.setColorAt(0.5, kShimmerPeak);
    grad.setColorAt(1.0, kShimmerEdge);

    p.fillRect(rect(), grad);
    p.restore();
}

} // namespace gitbolt::widgets
