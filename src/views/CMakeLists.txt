add_library(gitbolt_views STATIC
    src/main_window.cpp
    src/repository_view.cpp
    src/dashboard_view.cpp
    src/settings_dialog.cpp
    src/clone_dialog.cpp
)
target_include_directories(gitbolt_views PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(gitbolt_views PUBLIC gitbolt_widgets gitbolt_services Qt6::Widgets)
gitbolt_set_warnings(gitbolt_views)
