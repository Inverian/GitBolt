file(GLOB SOURCES "*.cpp" "*.h")
add_library(gitbolt_util STATIC ${SOURCES})

target_include_directories(gitbolt_util PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(gitbolt_util PUBLIC Qt6::Core Qt6::Concurrent)
gitbolt_set_warnings(gitbolt_util)
