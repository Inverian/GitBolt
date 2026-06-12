# Explicit source list — file(GLOB) hid new/deleted files from
# ninja until someone manually re-ran CMake, which is exactly
# how stale-binary "my change does nothing" sessions happen.
# Add new files HERE.
set(SOURCES
    AsyncRunner.cpp
    CrashHandler.cpp
    AsyncRunner.h
    CrashHandler.h
    LruCache.h
    PerformanceTimer.h
)
add_library(gitbolt_util STATIC ${SOURCES})

target_include_directories(gitbolt_util PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(gitbolt_util PUBLIC Qt6::Core Qt6::Concurrent)
gitbolt_set_warnings(gitbolt_util)
