file(GLOB SOURCES "*.cpp" "*.h")
add_library(gitbolt_watcher STATIC ${SOURCES})

target_include_directories(gitbolt_watcher PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(gitbolt_watcher PUBLIC Qt6::Core)
gitbolt_set_warnings(gitbolt_watcher)
