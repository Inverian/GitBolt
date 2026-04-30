#pragma once

#include "git/ObjectId.h"
#include "git/Signature.h"

#include <string>

namespace gitbolt::git {

/// One row in a ref's reflog. Mirrors libgit2's
/// `git_reflog_entry` shape: every action that moved the ref
/// (commit, reset, rebase, checkout, etc.) writes one of these
/// to `.git/logs/<refname>`.
///
/// The pair (oldId, newId) tells you what the ref was pointing
/// at before and after the action. For the very first entry on
/// a ref, oldId is the zero ObjectId.
struct ReflogEntry {
    ObjectId  oldId;
    ObjectId  newId;
    Signature committer;
    std::string message;  ///< e.g. "commit: foo" or "checkout: moving from..."
};

} // namespace gitbolt::git
