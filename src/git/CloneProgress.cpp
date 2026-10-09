#include "git/CloneProgress.h"

#include <cstddef>
#include <limits>
#include <optional>

namespace gitbolt::git {

namespace {

// git's failures are short — "fatal: …" plus a hint or two, with
// ssh's "…: Permission denied (publickey)." or a server's "remote: …"
// line in front — so a handful of lines always holds the cause.
constexpr std::size_t kMaxMessages = 5;

// No real record comes close; the cap only stops a server that never
// sends a line terminator from growing the buffer without bound.
constexpr std::size_t kMaxRecord = 4096;

bool consume(std::string_view& s, std::string_view prefix)
{
    if (s.substr(0, prefix.size()) != prefix)
        return false;
    s.remove_prefix(prefix.size());
    return true;
}

void trimSpaces(std::string_view& s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
        s.remove_suffix(1);
}

// Drop ANSI CSI sequences — git appends "\x1b[K" (clear to end of
// line) to sideband text when it believes stderr is a terminal.
std::string stripEscapes(std::string_view in)
{
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '\x1b' && i + 1 < in.size() && in[i + 1] == '[') {
            // Parameter bytes, then one final byte in '@'..'~'; the
            // loop's ++i steps past the final byte.
            i += 2;
            while (i < in.size() && !(in[i] >= '@' && in[i] <= '~'))
                ++i;
            continue;
        }
        out.push_back(in[i]);
    }
    return out;
}

// Leading decimal digits of `s`, consumed; nullopt when there are none.
std::optional<uint64_t> takeNumber(std::string_view& s)
{
    std::size_t n = 0;
    uint64_t value = 0;
    while (n < s.size() && s[n] >= '0' && s[n] <= '9') {
        value = value * 10 + static_cast<uint64_t>(s[n] - '0');
        ++n;
    }
    if (n == 0)
        return std::nullopt;
    s.remove_prefix(n);
    return value;
}

uint32_t clamp32(uint64_t v)
{
    return v > std::numeric_limits<uint32_t>::max()
        ? std::numeric_limits<uint32_t>::max()
        : static_cast<uint32_t>(v);
}

// "44.25 KiB" / "512 bytes" → bytes. This is git's own formatting
// (strbuf_humanise): binary units and always '.' as the decimal
// point, whatever the locale.
std::optional<uint64_t> parseSize(std::string_view s)
{
    const auto whole = takeNumber(s);
    if (!whole)
        return std::nullopt;
    uint64_t frac = 0;
    uint64_t scale = 1;
    if (consume(s, ".")) {
        while (!s.empty() && s.front() >= '0' && s.front() <= '9') {
            if (scale < 1000000) {
                frac = frac * 10 + static_cast<uint64_t>(s.front() - '0');
                scale *= 10;
            }
            s.remove_prefix(1);
        }
    }
    if (!consume(s, " "))
        return std::nullopt;
    uint64_t unit = 0;
    if (s == "GiB")
        unit = 1ull << 30;
    else if (s == "MiB")
        unit = 1ull << 20;
    else if (s == "KiB")
        unit = 1ull << 10;
    else if (s == "bytes" || s == "byte")
        unit = 1;
    else
        return std::nullopt;
    return *whole * unit + frac * unit / scale;
}

struct ProgressLine {
    std::string_view title;
    uint64_t current = 0;
    uint64_t total = 0;                 // 0: git doesn't know it
    std::optional<uint64_t> bytes;      // "Receiving objects" only
};

// The shape of every git progress line, whatever its title:
//
//   <title>: <pct>% (<n>/<total>)[, <size> | <rate>][, done.]
//   <title>: <n>[, done.]
//
// Titles that are really message prefixes are excluded so an error
// can never be mistaken for progress and swallowed.
std::optional<ProgressLine> parseProgressLine(std::string_view s)
{
    const auto colon = s.find(": ");
    if (colon == std::string_view::npos || colon == 0)
        return std::nullopt;
    ProgressLine p;
    p.title = s.substr(0, colon);
    if (p.title == "fatal" || p.title == "error" || p.title == "warning"
        || p.title == "hint" || p.title == "ERROR")
        return std::nullopt;
    s.remove_prefix(colon + 2);
    while (!s.empty() && s.front() == ' ')    // git pads: "Counting:   5%"
        s.remove_prefix(1);

    const auto first = takeNumber(s);
    if (!first)
        return std::nullopt;
    if (consume(s, "% (")) {
        const auto current = takeNumber(s);
        if (!current || !consume(s, "/"))
            return std::nullopt;
        const auto total = takeNumber(s);
        if (!total || !consume(s, ")"))
            return std::nullopt;
        p.current = *current;
        p.total = *total;
    } else {
        p.current = *first;     // total unknown, e.g. "Enumerating objects: 812"
    }

    if (s.empty())
        return p;
    if (!consume(s, ", "))
        return std::nullopt;
    // ", 1.20 MiB | 2.30 MiB/s, done." — the size runs up to the rate.
    std::string_view size = s.substr(0, s.find_first_of("|,"));
    trimSpaces(size);
    p.bytes = parseSize(size);      // nullopt for a bare "done."
    return p;
}

} // namespace

std::vector<CloneProgress> CloneProgressParser::feed(std::string_view chunk)
{
    std::vector<CloneProgress> out;
    while (!chunk.empty()) {
        // '\r' ends an in-place update, '\n' a finished line; CRLF just
        // yields an empty record in between, which parseRecord skips.
        const auto end = chunk.find_first_of("\r\n");
        // pending_ never exceeds kMaxRecord, so the subtraction is safe.
        pending_.append(chunk.substr(0, end).substr(0, kMaxRecord - pending_.size()));
        if (end == std::string_view::npos)
            break;
        parseRecord(pending_, out);
        pending_.clear();
        chunk.remove_prefix(end + 1);
    }
    return out;
}

std::vector<CloneProgress> CloneProgressParser::finish()
{
    std::vector<CloneProgress> out;
    if (!pending_.empty()) {
        parseRecord(pending_, out);
        pending_.clear();
    }
    return out;
}

std::string CloneProgressParser::messages() const
{
    std::string joined;
    for (const auto& m : messages_) {
        if (!joined.empty())
            joined += '\n';
        joined += m;
    }
    return joined;
}

void CloneProgressParser::parseRecord(std::string_view raw,
                                      std::vector<CloneProgress>& out)
{
    const std::string cleaned = stripEscapes(raw);
    std::string_view line = cleaned;
    trimSpaces(line);   // sideband text is padded with spaces
    if (line.empty())
        return;

    std::string_view body = line;
    const bool remote = consume(body, "remote:");
    if (remote) {
        trimSpaces(body);
        if (body.empty())
            return;     // a blank sideband line
    }

    if (!remote && body.substr(0, 13) == "Cloning into ") {
        out.push_back(state_);      // under way; nothing received yet
        return;
    }
    // "remote: Total 812 (delta 505), reused 0 …" — the server's pack
    // summary. Not progress, but no use in an error message either.
    if (remote && body.substr(0, 6) == "Total ")
        return;

    if (const auto p = parseProgressLine(body)) {
        if (remote) {
            // The server is still enumerating / counting / compressing:
            // connected, nothing received yet.
            out.push_back(state_);
            return;
        }
        if (p->title == "Receiving objects" || p->title == "Unpacking objects") {
            state_.phase = CloneProgress::Phase::Receiving;
            // index-pack indexes each object as it arrives.
            state_.receivedObjects = clamp32(p->current);
            state_.indexedObjects = state_.receivedObjects;
            state_.totalObjects = clamp32(p->total);
            if (p->bytes)
                state_.receivedBytes = *p->bytes;
        } else if (p->title == "Resolving deltas") {
            state_.phase = CloneProgress::Phase::Resolving;
            state_.indexedDeltas = clamp32(p->current);
            state_.totalDeltas = clamp32(p->total);
        } else if (p->title == "Updating files" || p->title == "Checking out files"
                   || p->title == "Filtering content") {
            // "Checking out files" is the pre-2.21 title; "Filtering
            // content" is the delayed (e.g. LFS) part of the checkout.
            state_.phase = CloneProgress::Phase::CheckingOut;
            state_.completedSteps = clamp32(p->current);
            state_.totalSteps = clamp32(p->total);
        } else {
            return;     // e.g. "Checking connectivity" — no phase of its own
        }
        out.push_back(state_);
        return;
    }

    // git's exit handler prints this warning when it keeps a clone
    // whose checkout failed. Not from a "remote:" line: that is the
    // server talking.
    if (!remote && line.find("Clone succeeded, but checkout failed") != std::string_view::npos)
        checkoutFailed_ = true;

    messages_.emplace_back(line);
    while (messages_.size() > kMaxMessages)
        messages_.pop_front();
}

} // namespace gitbolt::git
