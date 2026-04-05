add_library(gitbolt_plugins STATIC
    src/plugin_manager.cpp
)
target_include_directories(gitbolt_plugins PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(gitbolt_plugins PUBLIC gitbolt_core Qt6::Widgets)
gitbolt_set_warnings(gitbolt_plugins)
