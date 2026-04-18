#!/usr/bin/env python3
"""
tools/generate-icon.py — render the GitBolt master icon at 1024x1024
as a dark rounded plate with a yellow lightning bolt that's filled
with a grid of binary 0s and 1s.

Output:
    resources/icons/gitbolt-1024.png

After running this, regenerate the iconset variants and repack the
icns:

    python3 - << 'PY'
    # ...the iconset script that scales gitbolt-1024.png to all
    # ten sizes at the 824/1024 Apple HIG content area
    PY
    iconutil -c icns resources/icons/gitbolt.iconset \
                  -o resources/icons/gitbolt.icns

The script is deterministic — same output every run — so re-running
during design iteration is safe.

Design constants
----------------
* Canvas:        1024 x 1024
* Plate inset:   100 px each side  → plate is 824 x 824 (Apple HIG)
* Plate radius:  185 px (matches macOS Big Sur+ squircle approx)
* Plate color:   #1C2129  (matches the previous master)
* Bolt color:    #FFB81C  (golden yellow, matches the previous master)
* Bolt scale:    85% of the plate (much larger than the original ~55%)
* Bolt polygon:  7-vertex Font-Awesome-style lightning bolt
* Digit font:    SF Mono Bold at the master size, deterministic seed

Why a bolt outline + digit fill (not just digits)
-------------------------------------------------
At 16x16 / 32x32 the digit grid downsamples into noise that no
longer reads as a lightning bolt. We draw a thin yellow outline of
the bolt polygon FIRST, then overlay the digit fill. The outline
preserves the silhouette at every size; the digits show through
clearly only at 128x128 and above.
"""

from PIL import Image, ImageDraw, ImageFont
import os
import random

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------
CANVAS       = 1024
PLATE_INSET  = 100
PLATE_SIZE   = CANVAS - 2 * PLATE_INSET   # 824
PLATE_RADIUS = 185

PLATE_COLOR = (28, 33, 41, 255)     # #1C2129
BOLT_COLOR  = (255, 184, 28, 255)   # #FFB81C

# Bolt occupies 85% of the plate, centered. Larger than the
# original master, where it sat at roughly 55%.
BOLT_SCALE  = 0.85
BOLT_W      = int(PLATE_SIZE * BOLT_SCALE)
BOLT_H      = int(PLATE_SIZE * BOLT_SCALE)
BOLT_X      = PLATE_INSET + (PLATE_SIZE - BOLT_W) // 2
BOLT_Y      = PLATE_INSET + (PLATE_SIZE - BOLT_H) // 2

# Outline stroke width (1024 master). Scales down with the icon.
OUTLINE_PX  = 14

# 7-vertex lightning bolt polygon, normalized to 0..1, traced from
# the Font Awesome "bolt" glyph but balanced so the bottom tip is
# slightly less off-center than the vanilla version.
BOLT_POLY_NORM = [
    (0.42, 0.00),   # top-left
    (0.04, 0.55),   # left edge at the notch
    (0.40, 0.55),   # right edge of upper notch
    (0.18, 1.00),   # bottom tip
    (0.96, 0.42),   # right edge at upper-mid (going back up)
    (0.55, 0.42),   # left edge of lower notch
    (0.78, 0.00),   # top-right
]


def to_canvas_coords(poly_norm):
    return [
        (BOLT_X + int(round(x * BOLT_W)),
         BOLT_Y + int(round(y * BOLT_H)))
        for (x, y) in poly_norm
    ]


def load_font(size):
    """SF Mono Bold preferred; fall back to Menlo Bold."""
    candidates = [
        ("/System/Library/Fonts/SFNSMono.ttf", 0),
        ("/System/Library/Fonts/Menlo.ttc",    1),  # Bold variant
        ("/System/Library/Fonts/Menlo.ttc",    0),
    ]
    for path, idx in candidates:
        if os.path.exists(path):
            try:
                return ImageFont.truetype(path, size, index=idx)
            except Exception:
                continue
    return ImageFont.load_default()


def main():
    # 1. Build the canvas + dark rounded plate ----------------------
    canvas = Image.new("RGBA", (CANVAS, CANVAS), (0, 0, 0, 0))
    draw = ImageDraw.Draw(canvas)
    draw.rounded_rectangle(
        [PLATE_INSET, PLATE_INSET,
         CANVAS - PLATE_INSET, CANVAS - PLATE_INSET],
        radius=PLATE_RADIUS,
        fill=PLATE_COLOR,
    )

    # 2. Build the bolt mask ----------------------------------------
    bolt_poly = to_canvas_coords(BOLT_POLY_NORM)
    mask = Image.new("L", (CANVAS, CANVAS), 0)
    mask_draw = ImageDraw.Draw(mask)
    mask_draw.polygon(bolt_poly, fill=255)

    # 3. Solid yellow bolt under the digits — gives the silhouette
    #    something to render at small sizes where the digit grid
    #    blurs into noise. Drawn at ~35% alpha so the digits on top
    #    are still the dominant feature at large sizes.
    underlay = Image.new("RGBA", (CANVAS, CANVAS), (0, 0, 0, 0))
    udraw = ImageDraw.Draw(underlay)
    soft = (BOLT_COLOR[0], BOLT_COLOR[1], BOLT_COLOR[2], 90)
    udraw.polygon(bolt_poly, fill=soft)

    # Bolt outline (crisp yellow stroke around the perimeter so the
    # silhouette survives even at 16x16).
    udraw.line(bolt_poly + [bolt_poly[0]],
               fill=BOLT_COLOR, width=OUTLINE_PX, joint="curve")
    canvas = Image.alpha_composite(canvas, underlay)

    # 4. Render a grid of 0/1 digits across the bolt area -----------
    text_layer = Image.new("RGBA", (CANVAS, CANVAS), (0, 0, 0, 0))
    tdraw = ImageDraw.Draw(text_layer)

    font_size = 60
    font = load_font(font_size)
    char_bbox = tdraw.textbbox((0, 0), "0", font=font)
    char_w = (char_bbox[2] - char_bbox[0])
    char_h = (char_bbox[3] - char_bbox[1])
    # Tight grid spacing — leave a few px between rows/cols so the
    # digits read individually rather than blurring into a column.
    cell_w = char_w + 14
    cell_h = char_h + 18

    rng = random.Random(0xC0DE)  # deterministic
    # Cover the entire plate (not just the bolt bbox) so digits
    # near the bolt's diagonal edges don't get clipped by the row
    # iteration.
    for cy in range(PLATE_INSET - cell_h,
                    CANVAS - PLATE_INSET + cell_h, cell_h):
        for cx in range(PLATE_INSET - cell_w,
                        CANVAS - PLATE_INSET + cell_w, cell_w):
            digit = rng.choice("01")
            tdraw.text((cx, cy), digit, font=font, fill=BOLT_COLOR)

    # 5. Mask the digit layer to the bolt shape ---------------------
    masked = Image.new("RGBA", (CANVAS, CANVAS), (0, 0, 0, 0))
    masked.paste(text_layer, (0, 0), mask=mask)

    # 6. Composite onto the plate -----------------------------------
    canvas = Image.alpha_composite(canvas, masked)

    # 7. Save as the new master -------------------------------------
    out = "/Users/admin/Developer/GitBolt/resources/icons/gitbolt-1024.png"
    canvas.save(out, "PNG")
    print(f"wrote {out}")
    print(f"  plate:    {PLATE_SIZE}x{PLATE_SIZE} @ ({PLATE_INSET},{PLATE_INSET})")
    print(f"  bolt bbox:{BOLT_W}x{BOLT_H} @ ({BOLT_X},{BOLT_Y})")
    print(f"  bolt vs plate: {BOLT_SCALE * 100:.0f}%")


if __name__ == "__main__":
    main()
