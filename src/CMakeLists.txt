# Build order follows dependency graph (foundations first).
#
# Order matters for configure-time target resolution: if A's CMakeLists
# references `target_link_libraries(A PUBLIC B)`, then B must be defined
# first via add_subdirectory(B). Ninja figures out build-time order
# from the link graph, but CMake's configure phase needs targets to
# already exist.
#
# Current link graph:
#   util, conf, watcher — foundation, no cross-deps
#   telemetry → util (TestSession: one test-session predicate, shared
#                     with the crash handler); Core, Network
#   git → util (for caches)
#   models → git
#   services → git, models, util, watcher
#   editor → models
#   widgets → models, services, editor
#   dialogs → widgets, services, conf
#   ui → widgets, editor, services, conf, dialogs, telemetry
#        (MainWindow opens AboutDialog etc., and feeds usage statistics)
#   plugins → git (loaded by app, depends only on core)
#   app → everything
add_subdirectory(util)
add_subdirectory(conf)
add_subdirectory(watcher)
add_subdirectory(telemetry)
add_subdirectory(git)
add_subdirectory(models)
add_subdirectory(services)
add_subdirectory(editor)
add_subdirectory(widgets)
add_subdirectory(dialogs)   # must come before ui
add_subdirectory(ui)

if(GITBOLT_BUILD_PLUGINS)
    endif()

add_subdirectory(app)
