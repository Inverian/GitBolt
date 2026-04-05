file(GLOB SOURCES "*.cpp" "*.h")
add_library(gitbolt_services STATIC ${SOURCES})

target_include_directories(gitbolt_services PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(gitbolt_services PUBLIC
    gitbolt_git
    gitbolt_models
    gitbolt_util
    gitbolt_watcher
    Qt6::Widgets
    Qt6::Concurrent
)
gitbolt_set_warnings(gitbolt_services)
