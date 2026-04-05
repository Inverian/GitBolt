#pragma once

#include "gitbolt/core/object_id.h"
#include "gitbolt/core/signature.h"

#include <string>

namespace gitbolt::core {

struct StashEntry {
    size_t index;
    ObjectId id;
    std::string message;
    Signature author;
};

} // namespace gitbolt::core
