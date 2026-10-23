#pragma once

#include <QString>

#include <string_view>

namespace gitbolt::util {

/// How much of a commit's message (or of a reflog entry's, a tag's or a
/// stash's) the views lay out.
///
/// A message is text from the repository, of any size: one line of
/// megabytes makes a valid commit, and a clone brings it along. Qt lays
/// a paragraph out whole before it wraps or elides it, on the GUI
/// thread, in time that grows faster than the paragraph: 64K letters
/// with no space between them took 0.2 s in a text browser and 1M took
/// 30 s, and 64K mixed left-to-right and right-to-left letters 2.6 s.
/// Selecting a commit whose subject was such a line froze the window
/// for minutes, and a list repainting one stalled on every paint. So
/// the views get at most this much of a message, and say so where they
/// cut it ("…" at the end of a cut line).

/// One line in a list, table, tree or label: wider than any screen.
inline constexpr qsizetype kMaxLineChars = 1000;
/// One paragraph laid out wrapped: the Commit tab's subject, each line
/// of its body, each line of an editor's text.
inline constexpr qsizetype kMaxParagraphChars = 4096;
/// The whole of a message laid out at once: the Commit tab's body, an
/// editor's text, a tooltip.
inline constexpr qsizetype kMaxTextChars = 64 * 1024;

/// `text` as it is when it has at most `max` characters (UTF-16 units),
/// else its first `max` and "…".
QString elided(const QString& text, qsizetype max = kMaxLineChars);
/// The same for UTF-8 text, decoding no more of it than is shown.
QString elided(std::string_view utf8, qsizetype max = kMaxLineChars);

/// `text` as it is when no line of it is longer than `maxLine` and it
/// has at most `maxText` characters in all; else with each longer line
/// elided(), and what that leaves cut at `maxText`, then "…". A line of
/// megabytes amid ordinary ones leaves the lines after it in.
QString elidedText(const QString& text, qsizetype maxLine = kMaxParagraphChars,
                   qsizetype maxText = kMaxTextChars);
/// The same for UTF-8 text, decoding no more of it than is shown.
QString elidedText(std::string_view utf8, qsizetype maxLine = kMaxParagraphChars,
                   qsizetype maxText = kMaxTextChars);

} // namespace gitbolt::util
