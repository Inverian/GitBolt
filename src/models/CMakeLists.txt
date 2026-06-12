# Explicit source list — file(GLOB) hid new/deleted files from
# ninja until someone manually re-ran CMake, which is exactly
# how stale-binary "my change does nothing" sessions happen.
# Add new files HERE.
set(SOURCES
    BlameModel.cpp
    BranchModel.cpp
    CommitLogModel.cpp
    FileStatusModel.cpp
    BlameModel.h
    BranchModel.h
    CommitLogModel.h
    FileStatusModel.h
)
add_library(gitbolt_models STATIC ${SOURCES})

target_include_directories(gitbolt_models PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(gitbolt_models PUBLIC gitbolt_git gitbolt_util Qt6::Widgets)
gitbolt_set_warnings(gitbolt_models)
