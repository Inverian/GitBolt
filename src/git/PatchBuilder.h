#pragma once

#include "git/Diff.h"

#include <set>
#include <string>

namespace gitbolt::git {

/// Reconstruct a minimal, `git apply`-able unified patch for a single
/// hunk of a diffed file. The output carries the file header
/// (`diff --git` + `---`/`+++`) and exactly one hunk, so applying it
/// with `git apply --cached` stages just that hunk (and with
/// `--reverse` unstages it). Only meaningful for Modified files —
/// Added/Deleted files are a single all-or-nothing hunk where
/// file-level staging is equivalent.
///
/// Returns an empty string when the inputs are out of range or the
/// file is binary.
std::string buildHunkPatch(const DiffFileEntry& file, size_t hunkIndex);

/// Like buildHunkPatch but for a SUBSET of the hunk's lines (indices
/// into `hunks[hunkIndex].lines`). Unselected deletions degrade to
/// context (the line stays present), unselected additions are
/// dropped, and the hunk header's line counts are recomputed to
/// match the rewritten body. Selected indices that point at context
/// lines are ignored — context is always emitted.
///
/// Returns an empty string when the selection produces no effective
/// change, when the hunk contains end-of-file-newline markers
/// (rewriting those correctly per-side is not supported), or when
/// inputs are out of range.
std::string buildLinesPatch(const DiffFileEntry& file, size_t hunkIndex,
                            const std::set<size_t>& lineIndices);

} // namespace gitbolt::git
