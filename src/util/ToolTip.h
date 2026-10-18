#pragma once

#include <QString>

namespace gitbolt::util {

/// `text` as a tooltip that shows exactly that text. Qt has no plain
/// text setting for tooltips: it shows one as rich text whenever
/// Qt::mightBeRichText() takes it for markup, and so a branch or file
/// name, a tag message, a submodule's URL or a server's error message
/// holding "<b>…</b>" or "<a href=…>" was rendered, the repository or
/// the remote styling, hiding or adding to what the user read.
///
/// Text that Qt doesn't take for markup comes back as it is, and is
/// shown as plain text, as it always was. Only text that it does take
/// for markup is escaped, in a paragraph with nothing left in it to
/// interpret. That paragraph is white-space: pre, so the tooltip keeps
/// the text's lines as plain text would: rich text collapses line
/// breaks and runs of spaces (git's messages are laid out in lines),
/// and a tooltip that wraps rich text breaks every line it can into a
/// narrow block. Escaped whatever it held, an ordinary name or path or
/// a git error would be drawn that way too. Empty stays empty: no
/// tooltip.
///
/// For every tooltip that carries text GitBolt didn't write. One that
/// is GitBolt's own words, markup included, is set as it is.
QString plainToolTip(const QString& text);

} // namespace gitbolt::util
