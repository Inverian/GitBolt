#pragma once

#include <QColor>
#include <QPalette>

namespace gitbolt::widgets {

/// The amber of the note bars along the window (RepoOperationBar,
/// GitNoticeBar), light and dark: a bar has to stand out from the view
/// under it without looking like an error.
struct NoteBarColors {
    QColor background;
    QColor text;
    QColor border;   // the line along its bottom
};

inline NoteBarColors noteBarColors(const QPalette& palette) {
    const bool dark = palette.color(QPalette::WindowText).lightness()
                      > palette.color(QPalette::Window).lightness();
    if (dark)
        return {QColor(0x3a, 0x30, 0x19), QColor(0xf0, 0xd4, 0x8a), QColor(0x6b, 0x5a, 0x2a)};
    return {QColor(0xff, 0xf4, 0xd6), QColor(0x5c, 0x44, 0x00), QColor(0xe8, 0xc7, 0x66)};
}

} // namespace gitbolt::widgets
