file(GLOB SOURCES "*.cpp" "*.h")
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
