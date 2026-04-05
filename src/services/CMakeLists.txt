add_library(gitbolt_services STATIC
    src/git_service.cpp
    src/async_runner.cpp
    src/file_watcher.cpp
    src/search_service.cpp
    src/repo_manager.cpp
    src/settings_service.cpp
    src/theme_service.cpp
)
target_include_directories(gitbolt_services PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(gitbolt_services PUBLIC gitbolt_core gitbolt_models Qt6::Widgets Qt6::Concurrent)
gitbolt_set_warnings(gitbolt_services)
