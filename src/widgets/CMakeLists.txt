file(GLOB SOURCES "*.cpp" "*.h")
add_library(gitbolt_widgets STATIC ${SOURCES})

target_include_directories(gitbolt_widgets PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(gitbolt_widgets PUBLIC
    gitbolt_models
    gitbolt_services
    gitbolt_editor
    Qt6::Widgets
)
gitbolt_set_warnings(gitbolt_widgets)
