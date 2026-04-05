add_subdirectory(core)
add_subdirectory(models)
add_subdirectory(services)
add_subdirectory(widgets)
add_subdirectory(views)

if(GITBOLT_BUILD_PLUGINS)
    add_subdirectory(plugins)
endif()

add_subdirectory(app)
