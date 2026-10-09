#pragma once

#include "git/ObjectId.h"
#include "git/Signature.h"

#include <string>

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

} // namespace gitbolt::git
