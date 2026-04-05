qt_add_executable(gitbolt
    main.cpp
    resources/gitbolt.qrc
)

target_link_libraries(gitbolt PRIVATE
    gitbolt_ui
    gitbolt_dialogs
    gitbolt_widgets
    gitbolt_editor
    gitbolt_services
    gitbolt_models
    gitbolt_git
    gitbolt_conf
    gitbolt_watcher
    gitbolt_util
    gitbolt_plugins
    Qt6::Widgets
    Qt6::Concurrent
)

set_target_properties(gitbolt PROPERTIES
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
    OUTPUT_NAME "GitBolt"
)

if(APPLE)
    set_target_properties(gitbolt PROPERTIES
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.gitbolt.app"
        MACOSX_BUNDLE_BUNDLE_NAME "GitBolt"
        MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
    )
endif()

install(TARGETS gitbolt
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
