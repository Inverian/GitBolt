file(GLOB SOURCES "*.cpp" "*.h")
add_library(gitbolt_conf STATIC ${SOURCES})

target_include_directories(gitbolt_conf PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(gitbolt_conf PUBLIC Qt6::Widgets)
gitbolt_set_warnings(gitbolt_conf)
