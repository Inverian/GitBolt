# Explicit source list — file(GLOB) hid new/deleted files from
# ninja until someone manually re-ran CMake, which is exactly
# how stale-binary "my change does nothing" sessions happen.
# Add new files HERE.
set(SOURCES
    SettingsService.cpp
    ThemeService.cpp
    SettingsService.h
    ThemeService.h
)
add_library(gitbolt_conf STATIC ${SOURCES})

target_include_directories(gitbolt_conf PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(gitbolt_conf PUBLIC Qt6::Widgets)
gitbolt_set_warnings(gitbolt_conf)
