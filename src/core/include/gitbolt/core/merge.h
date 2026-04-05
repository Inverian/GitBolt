#pragma once

#include "gitbolt/core/object_id.h"

#include <string>
#include <vector>

namespace gitbolt::core {

enum class MergePreference {
    Normal,
    FastForwardOnly,
    NoFastForward,
};

enum class MergeAnalysis {
    Normal,
    UpToDate,
    FastForward,
    Unborn,
};

struct MergeConflictEntry {
    std::string path;
    std::string ancestorContent;
    std::string oursContent;
    std::string theirsContent;
    ObjectId ancestorId;
    ObjectId oursId;
    ObjectId theirsId;
};

struct MergeResult {
    MergeAnalysis analysis;
    bool hasConflicts = false;
    std::vector<MergeConflictEntry> conflicts;
    ObjectId resultTreeId;
};

} // namespace gitbolt::core
