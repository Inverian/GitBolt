# Explicit source list — file(GLOB) hid new/deleted files from
# ninja until someone manually re-ran CMake, which is exactly
# how stale-binary "my change does nothing" sessions happen.
# Add new files HERE.
set(SOURCES
    Blame.cpp
    Branch.cpp
    Cherrypick.cpp
    Commit.cpp
    CommitCache.cpp
    Config.cpp
    Diff.cpp
    DiffCache.cpp
    Error.cpp
    GitProcess.cpp
    GitProcessLog.cpp
    Index.cpp
    Merge.cpp
    ObjectId.cpp
    PatchBuilder.cpp
    Rebase.cpp
    Remote.cpp
    Repository.cpp
    Revwalk.cpp
    Signature.cpp
    Stash.cpp
    Status.cpp
    Submodule.cpp
    Tag.cpp
    Worktree.cpp
    Blame.h
    Branch.h
    Cherrypick.h
    Commit.h
    CommitCache.h
    Config.h
    Diff.h
    DiffCache.h
    Error.h
    GitProcess.h
    GitProcessLog.h
    Index.h
    Merge.h
    ObjectId.h
    PatchBuilder.h
    Rebase.h
    Reflog.h
    Remote.h
    Repository.h
    Revwalk.h
    Signature.h
    Stash.h
    Status.h
    Submodule.h
    Tag.h
    Tree.h
    Worktree.h
)
add_library(gitbolt_git STATIC ${SOURCES})

target_include_directories(gitbolt_git PUBLIC ${CMAKE_SOURCE_DIR}/src)

# Link libgit2
if(TARGET PkgConfig::LIBGIT2)
    target_link_libraries(gitbolt_git PUBLIC PkgConfig::LIBGIT2)
elseif(TARGET libgit2::git2)
    target_link_libraries(gitbolt_git PUBLIC libgit2::git2)
elseif(TARGET git2)
    target_link_libraries(gitbolt_git PUBLIC git2)
endif()

target_link_libraries(gitbolt_git PUBLIC gitbolt_util)
target_link_libraries(gitbolt_git PRIVATE Qt6::Core)
gitbolt_set_warnings(gitbolt_git)
