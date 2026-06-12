# Explicit source list — file(GLOB) hid new/deleted files from
# ninja until someone manually re-ran CMake, which is exactly
# how stale-binary "my change does nothing" sessions happen.
# Add new files HERE.
set(SOURCES
    FileWatcher.cpp
    FileWatcher.h
)
add_library(gitbolt_watcher STATIC ${SOURCES})

target_include_directories(gitbolt_watcher PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(gitbolt_watcher PUBLIC Qt6::Core)
gitbolt_set_warnings(gitbolt_watcher)
