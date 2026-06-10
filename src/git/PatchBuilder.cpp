#include "git/PatchBuilder.h"

namespace gitbolt::git {

namespace {

// libgit2 hands us hunk headers and line content with the source
// newline still attached (content_len spans it). Patches are
// assembled line-by-line and joined with our own '\n', so strip
// any trailing newline bytes first.
std::string stripped(const std::string& s)
{
    std::string r = s;
    while (!r.empty() && (r.back() == '\n' || r.back() == '\r'))
        r.pop_back();
    return r;
}

// Shared file header. `git apply` does not need the `index ..`
// line; the a/ b/ prefixes match git's default diff output.
std::string fileHeader(const DiffFileEntry& file)
{
    const std::string& oldP =
        file.oldPath.empty() ? file.newPath : file.oldPath;
    const std::string& newP =
        file.newPath.empty() ? file.oldPath : file.newPath;
    std::string out;
    out += "diff --git a/" + oldP + " b/" + newP + "\n";
    out += "--- a/" + oldP + "\n";
    out += "+++ b/" + newP + "\n";
    return out;
}

const char* kNoNewlineMarker = "\\ No newline at end of file";

} // namespace

std::string buildHunkPatch(const DiffFileEntry& file, size_t hunkIndex)
{
    if (file.isBinary || hunkIndex >= file.hunks.size())
        return {};
    const DiffHunk& hunk = file.hunks[hunkIndex];

    std::string out = fileHeader(file);
    out += stripped(hunk.header) + "\n";

    for (const auto& line : hunk.lines) {
        switch (line.type) {
        case DiffLineType::Context:
            out += " " + stripped(line.content) + "\n";
            break;
        case DiffLineType::Addition:
            out += "+" + stripped(line.content) + "\n";
            break;
        case DiffLineType::Deletion:
            out += "-" + stripped(line.content) + "\n";
            break;
        case DiffLineType::ContextEOFNL:
        case DiffLineType::AddEOFNL:
        case DiffLineType::DelEOFNL:
            // The pseudo-line's stored content varies by libgit2
            // version; the patch format wants the literal marker.
            out += std::string(kNoNewlineMarker) + "\n";
            break;
        default:
            // FileHeader / HunkHeader / Binary never appear inside
            // hunk.lines for text diffs; skip defensively.
            break;
        }
    }
    return out;
}

std::string buildLinesPatch(const DiffFileEntry& file, size_t hunkIndex,
                            const std::set<size_t>& lineIndices)
{
    if (file.isBinary || hunkIndex >= file.hunks.size()
        || lineIndices.empty())
        return {};
    const DiffHunk& hunk = file.hunks[hunkIndex];

    // EOFNL markers interact with which side "owns" the final
    // newline; rewriting them correctly for an arbitrary subset of
    // lines is fiddly and rarely needed — refuse and let the caller
    // fall back to whole-hunk staging.
    for (const auto& line : hunk.lines) {
        if (line.type == DiffLineType::ContextEOFNL
            || line.type == DiffLineType::AddEOFNL
            || line.type == DiffLineType::DelEOFNL)
            return {};
    }

    // Rewrite the body: selected +/- lines keep their role;
    // unselected deletions degrade to context (the old line simply
    // stays); unselected additions vanish (they are not being
    // introduced). Counts are recomputed for the new header.
    std::string body;
    int oldCount = 0;
    int newCount = 0;
    bool effective = false;
    for (size_t i = 0; i < hunk.lines.size(); ++i) {
        const auto& line = hunk.lines[i];
        const bool selected = lineIndices.count(i) > 0;
        switch (line.type) {
        case DiffLineType::Context:
            body += " " + stripped(line.content) + "\n";
            ++oldCount;
            ++newCount;
            break;
        case DiffLineType::Deletion:
            if (selected) {
                body += "-" + stripped(line.content) + "\n";
                ++oldCount;
                effective = true;
            } else {
                body += " " + stripped(line.content) + "\n";
                ++oldCount;
                ++newCount;
            }
            break;
        case DiffLineType::Addition:
            if (selected) {
                body += "+" + stripped(line.content) + "\n";
                ++newCount;
                effective = true;
            }
            // Unselected additions are simply omitted.
            break;
        default:
            break;
        }
    }
    if (!effective)
        return {};

    std::string out = fileHeader(file);
    out += "@@ -" + std::to_string(hunk.oldStart) + ","
         + std::to_string(oldCount) + " +"
         + std::to_string(hunk.newStart) + ","
         + std::to_string(newCount) + " @@\n";
    out += body;
    return out;
}

} // namespace gitbolt::git
