#pragma once

#include "git/ObjectId.h"
#include "git/Signature.h"

#include <string>

namespace gitbolt::git {

struct StashEntry {
    size_t index;
    ObjectId id;
    std::string message;
    Signature author;
};

} // namespace gitbolt::git
