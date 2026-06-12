# Explicit source list — file(GLOB) hid new/deleted files from
# ninja until someone manually re-ran CMake, which is exactly
# how stale-binary "my change does nothing" sessions happen.
# Add new files HERE.
set(SOURCES
    DashboardView.cpp
    InlineOpIndicator.cpp
    MainWindow.cpp
    RepoCommandController.cpp
    RepositoryView.cpp
    DashboardView.h
    InlineOpIndicator.h
    MainWindow.h
    RepoCommandController.h
    RepositoryView.h
)
add_library(gitbolt_ui STATIC ${SOURCES})

target_include_directories(gitbolt_ui PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(gitbolt_ui PUBLIC
    gitbolt_widgets
    gitbolt_editor
    gitbolt_services
    gitbolt_conf
    gitbolt_dialogs   # MainWindow opens AboutDialog, SettingsDialog, etc.
    Qt6::Widgets
    Qt6::Svg     # SVG icon rendering for Commands menu (Material Symbols)
    Qt6::Concurrent  # QtConcurrent::run for async dashboard repo probes
)
gitbolt_set_warnings(gitbolt_ui)
