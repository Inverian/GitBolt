#pragma once

#include <cstdint>
#include <string>

namespace gitbolt::git {

enum class FileStatus : uint32_t {
    Current         = 0,
    IndexNew        = (1u << 0),
    IndexModified   = (1u << 1),
    IndexDeleted    = (1u << 2),
    IndexRenamed    = (1u << 3),
    IndexTypeChange = (1u << 4),
    WtNew           = (1u << 7),
    WtModified      = (1u << 8),
    WtDeleted       = (1u << 9),
    WtTypeChange    = (1u << 10),
    WtRenamed       = (1u << 11),
    WtUnreadable    = (1u << 12),
    Ignored         = (1u << 14),
    Conflicted      = (1u << 15),
};

inline FileStatus operator|(FileStatus a, FileStatus b) {
    return static_cast<FileStatus>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline bool hasFlag(FileStatus status, FileStatus flag) {
    return (static_cast<uint32_t>(status) & static_cast<uint32_t>(flag)) != 0;
}

struct StatusEntry {
    std::string path;
    std::string oldPath;
    FileStatus status;

    bool isStaged() const;
    bool isWorkingTree() const;
    bool isConflicted() const;
    bool isUntracked() const;
};

} // namespace gitbolt::git
