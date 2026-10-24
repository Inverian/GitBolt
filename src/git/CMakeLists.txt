# Explicit source list — file(GLOB) hid new/deleted files from
# ninja until someone manually re-ran CMake, which is exactly
# how stale-binary "my change does nothing" sessions happen.
# Add new files HERE.
set(SOURCES
    Blame.cpp
    Branch.cpp
    Cherrypick.cpp
    CloneProgress.cpp
    Commit.cpp
    CommitCache.cpp
    Config.cpp
    Diff.cpp
    DiffCache.cpp
    Error.cpp
    GitProcess.cpp
    GitProcessLog.cpp
    GitVersion.cpp
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
    Askpass.h
    Blame.h
    Branch.h
    Cherrypick.h
    CloneProgress.h
    Commit.h
    CommitCache.h
    Config.h
    Diff.h
    DiffCache.h
    Error.h
    GitProcess.h
    GitProcessLog.h
    GitVersion.h
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

# libgit2, system or bundled (cmake/Libgit2.cmake). First, so its
# headers precede any other <git2.h> that later dependencies' include
# dirs could bring (see src/app/CMakeLists.txt).
target_link_libraries(gitbolt_git PUBLIC gitbolt_libgit2)

target_link_libraries(gitbolt_git PUBLIC gitbolt_util)
target_link_libraries(gitbolt_git PRIVATE Qt6::Core)
# The advisories GitBolt judges the user's git by (GitVersion.h), as
# :/git/git-advisories.json. Data, not code: tools/check-ci-pins.py
# reads the same file.
qt_add_resources(gitbolt_git "git_advisories"
    PREFIX "/git"
    FILES git-advisories.json)
# lupdate (see the top-level CMakeLists) reads every source of the
# target, and would take the JSON for C++.
set_property(TARGET gitbolt_git APPEND PROPERTY QT_EXCLUDE_SOURCES_FROM_TRANSLATION
    ${CMAKE_CURRENT_SOURCE_DIR}/git-advisories.json)
gitbolt_set_warnings(gitbolt_git)
