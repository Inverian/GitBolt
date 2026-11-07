# Explicit source list — file(GLOB) hid new/deleted files from
# ninja until someone manually re-ran CMake, which is exactly
# how stale-binary "my change does nothing" sessions happen.
# Add new files HERE.
#
# In-app updates: asking GitHub for the latest release, downloading
# and checking its installer, and installing it over the running copy.
# No feed address here: main() decides whether this build checks at all
# (GITBOLT_UPDATES, official builds only) and fills Config; tests use a
# loopback stub. Network is PRIVATE, as in telemetry.
set(SOURCES
    Version.cpp
    Version.h
)
add_library(gitbolt_update STATIC ${SOURCES})

target_include_directories(gitbolt_update PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(gitbolt_update PUBLIC Qt6::Core gitbolt_util PRIVATE Qt6::Network)
gitbolt_set_warnings(gitbolt_update)
