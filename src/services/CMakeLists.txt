# Explicit source list — file(GLOB) hid new/deleted files from
# ninja until someone manually re-ran CMake, which is exactly
# how stale-binary "my change does nothing" sessions happen.
# Add new files HERE.
set(SOURCES
    GitService.cpp
    RecentRepoProbe.cpp
    RepoManager.cpp
    SearchService.cpp
    GitService.h
    RecentRepoProbe.h
    RepoManager.h
    SearchService.h
)
add_library(gitbolt_services STATIC ${SOURCES})

target_include_directories(gitbolt_services PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(gitbolt_services PUBLIC
    gitbolt_git
    gitbolt_models
    gitbolt_util
    gitbolt_watcher
    Qt6::Widgets
    Qt6::Concurrent
)
gitbolt_set_warnings(gitbolt_services)
