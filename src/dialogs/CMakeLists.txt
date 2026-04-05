file(GLOB SOURCES "*.cpp" "*.h")
add_library(gitbolt_dialogs STATIC ${SOURCES})

target_include_directories(gitbolt_dialogs PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(gitbolt_dialogs PUBLIC
    gitbolt_widgets
    gitbolt_services
    gitbolt_conf
    Qt6::Widgets
)
gitbolt_set_warnings(gitbolt_dialogs)
