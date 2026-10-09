# Explicit source list — file(GLOB) hid new/deleted files from
# ninja until someone manually re-ran CMake, which is exactly
# how stale-binary "my change does nothing" sessions happen.
# Add new files HERE.
set(SOURCES
    BlameWidget.cpp
    BoltLogoWidget.cpp
    BranchTreeWidget.cpp
    CommitFilterProxy.cpp
    CommitMessageEdit.cpp
    ConsoleOutputWidget.cpp
    DiffTextEdit.cpp
    DiffViewerWidget.cpp
    FileTreeWidget.cpp
    GitFlowWidget.cpp
    InteractiveRebaseWidget.cpp
    LoadingOverlayWidget.cpp
    MaintenanceWidget.cpp
    MergeConflictWidget.cpp
    RecordTablePanel.cpp
    RepoOperationBar.cpp
    RevisionGraphWidget.cpp
    SubmoduleWidget.cpp
    TerminalWidget.cpp
    WorktreeWidget.cpp
    BlameWidget.h
    BoltLogoWidget.h
    BranchTreeWidget.h
    CommitFilterProxy.h
    CommitMessageEdit.h
    ConsoleOutputWidget.h
    DiffTextEdit.h
    DiffViewerWidget.h
    FileTreeWidget.h
    GitFlowWidget.h
    InteractiveRebaseWidget.h
    LoadingOverlayWidget.h
    MaintenanceWidget.h
    MergeConflictWidget.h
    RecordTablePanel.h
    RepoOperationBar.h
    RevisionGraphWidget.h
    SubmoduleWidget.h
    TerminalWidget.h
    WorktreeWidget.h
)
add_library(gitbolt_widgets STATIC ${SOURCES})

target_include_directories(gitbolt_widgets PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(gitbolt_widgets PUBLIC
    gitbolt_models
    gitbolt_services
    gitbolt_editor
    Qt6::Widgets
)
gitbolt_set_warnings(gitbolt_widgets)

# TerminalWidget's forkpty: in libc on macOS and glibc >= 2.34, but in
# libutil on older glibc (an empty stub on newer, so always safe).
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(gitbolt_widgets PRIVATE util)
endif()

# TerminalWidget's Windows backend (ConPTY). Its own target, with no Qt
# on its include path, keeps the Win32 code Qt-free: it compiles and is
# tested (tests/widgets/TestConPtyProcess.cpp) without a widget.
if(WIN32)
    add_library(gitbolt_conpty STATIC
        ConPtyProcess.cpp
        ConPtyProcess.h
    )
    # Nothing for moc/uic/rcc here, but the project-wide AUTOMOC would
    # still run moc over it and compile an empty mocs_compilation.cpp.
    set_target_properties(gitbolt_conpty PROPERTIES
        AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
    target_include_directories(gitbolt_conpty PUBLIC ${CMAKE_SOURCE_DIR}/src)
    gitbolt_set_warnings(gitbolt_conpty)
    target_link_libraries(gitbolt_widgets PRIVATE gitbolt_conpty)
endif()
