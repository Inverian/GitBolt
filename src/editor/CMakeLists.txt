# Explicit source list — file(GLOB) hid new/deleted files from
# ninja until someone manually re-ran CMake, which is exactly
# how stale-binary "my change does nothing" sessions happen.
# Add new files HERE.
set(SOURCES
    DiffSyntaxHighlighter.cpp
    RevisionGraphDelegate.cpp
    DiffSyntaxHighlighter.h
    RevisionGraphDelegate.h
)
add_library(gitbolt_editor STATIC ${SOURCES})

target_include_directories(gitbolt_editor PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(gitbolt_editor PUBLIC gitbolt_models Qt6::Widgets)
gitbolt_set_warnings(gitbolt_editor)
