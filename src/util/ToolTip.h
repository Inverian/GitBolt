#pragma once

#include <QString>

namespace gitbolt::util {

/// `text` as a tooltip that shows exactly that text. Qt has no plain
/// text setting for tooltips: it shows one as rich text whenever
/// Qt::mightBeRichText() takes it for markup, and so a branch or file
/// name, a tag message, a submodule's URL or a server's error message
/// holding "<b>…</b>" or "<a href=…>" was rendered, the repository or
/// the remote styling, hiding or adding to what the user read. Escaped
/// and wrapped in a paragraph, the text always takes the rich text path
/// with nothing left in it to interpret; white-space: pre-wrap keeps
/// its line breaks and runs of spaces, which rich text would collapse
/// (git's messages are laid out in lines). Empty stays empty: no
/// tooltip.
///
/// For every tooltip that carries text GitBolt didn't write. One that
/// is GitBolt's own words, markup included, is set as it is.
QString plainToolTip(const QString& text);

} // namespace gitbolt::util
