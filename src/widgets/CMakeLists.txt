add_library(gitbolt_widgets STATIC
    src/revision_graph_widget.cpp
    src/revision_graph_delegate.cpp
    src/diff_viewer_widget.cpp
    src/diff_syntax_highlighter.cpp
    src/commit_editor_widget.cpp
    src/staging_widget.cpp
    src/blame_widget.cpp
    src/merge_conflict_widget.cpp
    src/branch_tree_widget.cpp
    src/search_bar_widget.cpp
    src/console_output_widget.cpp
)
target_include_directories(gitbolt_widgets PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(gitbolt_widgets PUBLIC gitbolt_models gitbolt_services Qt6::Widgets)
gitbolt_set_warnings(gitbolt_widgets)
