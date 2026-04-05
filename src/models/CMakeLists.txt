add_library(gitbolt_models STATIC
    src/commit_log_model.cpp
    src/file_status_model.cpp
    src/branch_model.cpp
    src/diff_model.cpp
    src/blame_model.cpp
    src/stash_model.cpp
    src/tag_model.cpp
    src/search_results_model.cpp
    src/reflog_model.cpp
)

target_include_directories(gitbolt_models PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(gitbolt_models PUBLIC gitbolt_core Qt6::Widgets)
gitbolt_set_warnings(gitbolt_models)
