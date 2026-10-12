# Explicit source list — file(GLOB) hid new/deleted files from
# ninja until someone manually re-ran CMake, which is exactly
# how stale-binary "my change does nothing" sessions happen.
# Add new files HERE.
#
# Usage statistics and crash reports (OpenPanel, GlitchTip). No
# endpoints here: they are compiled into the app target only, so no
# library or test binary can carry them. Network is PRIVATE: only
# TelemetryService.h is for other modules, and it needs no QtNetwork.
set(SOURCES
    CrashLogParser.cpp
    EventQueue.cpp
    EventSchema.cpp
    GlitchTipSink.cpp
    OpenPanelSink.cpp
    TelemetryConfig.cpp
    TelemetryGate.cpp
    TelemetryService.cpp
    Transport.cpp
    CrashLogParser.h
    EventQueue.h
    EventSchema.h
    GlitchTipSink.h
    OpenPanelSink.h
    TelemetryConfig.h
    TelemetryGate.h
    TelemetryService.h
    Transport.h
)
add_library(gitbolt_telemetry STATIC ${SOURCES})

target_include_directories(gitbolt_telemetry PUBLIC ${CMAKE_SOURCE_DIR}/src)
# util: the shared test-session and opt-out predicates (TestSession.h)
target_link_libraries(gitbolt_telemetry PUBLIC Qt6::Core gitbolt_util PRIVATE Qt6::Network)
gitbolt_set_warnings(gitbolt_telemetry)
