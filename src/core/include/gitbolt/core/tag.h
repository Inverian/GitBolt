#pragma once

#include "gitbolt/core/object_id.h"
#include "gitbolt/core/signature.h"

#include <string>

namespace gitbolt::core {

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

} // namespace gitbolt::core
