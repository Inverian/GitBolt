file(GLOB SOURCES "*.cpp" "*.h")
add_library(gitbolt_editor STATIC ${SOURCES})

target_include_directories(gitbolt_editor PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(gitbolt_editor PUBLIC gitbolt_models Qt6::Widgets)
gitbolt_set_warnings(gitbolt_editor)
