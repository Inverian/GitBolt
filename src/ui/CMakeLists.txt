file(GLOB SOURCES "*.cpp" "*.h")
add_library(gitbolt_ui STATIC ${SOURCES})

target_include_directories(gitbolt_ui PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(gitbolt_ui PUBLIC
    gitbolt_widgets
    gitbolt_editor
    gitbolt_services
    gitbolt_conf
    Qt6::Widgets
)
gitbolt_set_warnings(gitbolt_ui)
