#pragma once

#include "git/ObjectId.h"
#include "git/Signature.h"

#include <string>

namespace gitbolt::git {

enum class TagType {
    Lightweight,
    Annotated,
};

struct TagInfo {
    std::string name;
    ObjectId targetId;
    ObjectId tagId;
    TagType type;
    std::string message;
    Signature tagger;
};

} // namespace gitbolt::git
