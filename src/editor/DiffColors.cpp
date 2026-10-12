#include "editor/DiffColors.h"

#include <QPalette>

namespace gitbolt::editor {

DiffColors diffColorsFor(const QPalette& palette)
{
    const bool dark = palette.color(QPalette::WindowText).lightness()
                      > palette.color(QPalette::Window).lightness();
    if (dark) {
        return {
            .addBg = QColor(0x1f, 0x3d, 0x2a),
            .delBg = QColor(0x4b, 0x23, 0x26),
            .hunkBg = QColor(0x1d, 0x2d, 0x4a),
            .addFg = QColor(0x7e, 0xe7, 0x87),
            .delFg = QColor(0xff, 0xa1, 0x98),
            .hunkFg = QColor(0x79, 0xc0, 0xff),
            .contextFg = QColor(0x9e, 0x9e, 0x9e),
        };
    }
    return {
        .addBg = QColor(200, 255, 200),
        .delBg = QColor(255, 220, 220),
        .hunkBg = QColor(220, 230, 255),
        .addFg = QColor(0, 140, 0),
        .delFg = QColor(180, 0, 0),
        .hunkFg = QColor(0, 102, 204),
        .contextFg = QColor(128, 128, 128),
    };
}

} // namespace gitbolt::editor
