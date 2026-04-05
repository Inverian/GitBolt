file(GLOB SOURCES "*.cpp" "*.h")
add_library(gitbolt_models STATIC ${SOURCES})

target_include_directories(gitbolt_models PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(gitbolt_models PUBLIC gitbolt_git Qt6::Widgets)
gitbolt_set_warnings(gitbolt_models)
