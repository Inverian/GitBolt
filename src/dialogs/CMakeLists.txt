# Explicit source list — file(GLOB) hid new/deleted files from
# ninja until someone manually re-ran CMake, which is exactly
# how stale-binary "my change does nothing" sessions happen.
# Add new files HERE.
set(SOURCES
    AboutDialog.cpp
    AdvancedFilterDialog.cpp
    BranchPickerDialog.cpp
    CherryPickDialog.cpp
    CloneDialog.cpp
    CommitDialog.cpp
    GitFlowDialog.cpp
    MaintenanceDialog.cpp
    RebaseDialog.cpp
    ReflogDialog.cpp
    RemotesDialog.cpp
    SettingsDialog.cpp
    StashDialog.cpp
    StashManageDialog.cpp
    TagDialog.cpp
    TextEditorDialog.cpp
    WorktreeDialog.cpp
    AboutDialog.h
    AdvancedFilterDialog.h
    BranchPickerDialog.h
    CherryPickDialog.h
    CloneDialog.h
    CommitDialog.h
    GitFlowDialog.h
    MaintenanceDialog.h
    RebaseDialog.h
    ReflogDialog.h
    RemotesDialog.h
    SettingsDialog.h
    StashDialog.h
    StashManageDialog.h
    TagDialog.h
    TextEditorDialog.h
    WorktreeDialog.h
)
add_library(gitbolt_dialogs STATIC ${SOURCES})

target_include_directories(gitbolt_dialogs PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(gitbolt_dialogs PUBLIC
    gitbolt_widgets
    gitbolt_services
    gitbolt_conf
    Qt6::Widgets
)
gitbolt_set_warnings(gitbolt_dialogs)
