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
