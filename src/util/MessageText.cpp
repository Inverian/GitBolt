#include "util/MessageText.h"

#include <QStringView>

#include <algorithm>

namespace gitbolt::util {

namespace {

constexpr QChar kEllipsis(0x2026);

// The first `max` units of `text`, not ending between the two halves
// of a surrogate pair.
QStringView head(QStringView text, qsizetype max)
{
    QStringView cut = text.left(max);
    if (!cut.isEmpty() && cut.back().isHighSurrogate())
        cut.chop(1);
    return cut;
}

// As much of `utf8` as can show in `max` units, and more: a unit takes
// four bytes at most (one of a surrogate pair takes two; a replacement
// character, up to four), so when there is more text than this window
// it decodes to more than `max` units, and the caller cuts it. Decoding
// the whole of a message of megabytes, on every paint of a list, just
// to show its start, is what this saves.
QString decodeAtMost(std::string_view utf8, qsizetype max)
{
    const size_t window = static_cast<size_t>(max + 1) * 4;
    const size_t take = std::min(utf8.size(), window);
    return QString::fromUtf8(utf8.data(), static_cast<qsizetype>(take));
}

// `line` onto `out`, cut at `max` units; true when it was cut.
bool appendElided(QString& out, QStringView line, qsizetype max)
{
    if (line.size() <= max) {
        out += line;
        return false;
    }
    out += head(line, max);
    out += kEllipsis;
    return true;
}

bool appendElided(QString& out, std::string_view line, qsizetype max)
{
    const QString decoded = decodeAtMost(line, max);
    return appendElided(out, QStringView(decoded), max);
}

qsizetype newlineAt(QStringView text, qsizetype from)
{
    return text.indexOf(QLatin1Char('\n'), from);
}

qsizetype newlineAt(std::string_view text, qsizetype from)
{
    const size_t at = text.find('\n', static_cast<size_t>(from));
    return at == std::string_view::npos ? -1 : static_cast<qsizetype>(at);
}

QStringView slice(QStringView text, qsizetype from, qsizetype to)
{
    return text.sliced(from, to - from);
}

std::string_view slice(std::string_view text, qsizetype from, qsizetype to)
{
    return text.substr(static_cast<size_t>(from), static_cast<size_t>(to - from));
}

// Line by line, each cut at `maxLine`, until `maxText` units are out:
// a line of megabytes amid ordinary ones costs only the scan for its
// end, and the lines after it still show.
template <typename View>
QString elideLines(View text, qsizetype maxLine, qsizetype maxText, bool& changed)
{
    const auto size = static_cast<qsizetype>(text.size());
    QString out;
    qsizetype from = 0;
    for (;;) {
        const qsizetype end = newlineAt(text, from);
        changed |= appendElided(out, slice(text, from, end < 0 ? size : end), maxLine);
        if (end >= 0)
            out += QLatin1Char('\n');
        if (out.size() > maxText) {
            QString cut = head(out, maxText).toString();
            if (!cut.endsWith(kEllipsis))
                cut += kEllipsis;
            changed = true;
            return cut;
        }
        if (end < 0)
            return out;
        from = end + 1;
    }
}

} // namespace

QString elided(const QString& text, qsizetype max)
{
    if (text.size() <= max)
        return text;
    QString out = head(text, max).toString();
    out += kEllipsis;
    return out;
}

QString elided(std::string_view utf8, qsizetype max)
{
    return elided(decodeAtMost(utf8, max), max);
}

QString elidedText(const QString& text, qsizetype maxLine, qsizetype maxText)
{
    if (text.size() <= std::min(maxLine, maxText))
        return text;
    bool changed = false;
    QString out = elideLines(QStringView(text), maxLine, maxText, changed);
    return changed ? out : text;
}

QString elidedText(std::string_view utf8, qsizetype maxLine, qsizetype maxText)
{
    bool changed = false;
    return elideLines(utf8, maxLine, maxText, changed);
}

} // namespace gitbolt::util
