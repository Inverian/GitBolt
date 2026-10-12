#pragma once

#include "git/ObjectId.h"
#include "git/Signature.h"

#include <string>
#include <string_view>

namespace gitbolt::git {

enum class TagType {
    Lightweight,
    Annotated,
};

// Same naming contract as BranchInfo: `name` is the short form
// ("v1.0") that users see and that createTag / deleteTag take;
// `fullRefName` is the unambiguous "refs/tags/v1.0".
// `targetId` is what the tag names, peeled through any tag objects
// (normally a commit), for both kinds. `tagId`, `message` and
// `tagger` come from an annotated tag's tag object; a lightweight
// tag has none of them.
struct TagInfo {
    std::string name;
    std::string fullRefName;
    ObjectId targetId;
    ObjectId tagId;
    TagType type;
    std::string message;
    Signature tagger;
};

/// A signed tag's message without its signature block. A tag has no
/// signature header the way a commit has gpgsig: `git tag -s` appends
/// the armored block to the message itself, and libgit2 hands back
/// the whole body, so the PGP or SSH block used to show up in the
/// tag's tooltip. Same rule as git's parse_signed_buffer: the message
/// ends where the LAST line that starts with a signature marker
/// begins. A message with no such line comes back whole.
std::string_view tagMessageWithoutSignature(std::string_view message);

} // namespace gitbolt::git
