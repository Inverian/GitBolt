file(GLOB SOURCES "*.cpp" "*.h")
add_library(gitbolt_plugins STATIC ${SOURCES})

target_include_directories(gitbolt_plugins PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(gitbolt_plugins PUBLIC
    gitbolt_git
    gitbolt_conf
    Qt6::Widgets
)
gitbolt_set_warnings(gitbolt_plugins)
