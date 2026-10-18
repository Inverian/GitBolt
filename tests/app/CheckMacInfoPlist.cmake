# CheckMacInfoPlist — the Info.plist keys Finder's "Open in GitBolt"
# and folder open events depend on (src/app/platform/macos/
# Info.plist.in). macOS reads them only when it registers the app, and
# a mistake in one fails silently: the item just never shows. Run on
# the built bundle by ctest (macos_info_plist) and on the DMG's by CI:
#
#   cmake -DPLIST=<GitBolt.app>/Contents/Info.plist -P CheckMacInfoPlist.cmake

if(NOT PLIST OR NOT EXISTS "${PLIST}")
    message(FATAL_ERROR "CheckMacInfoPlist: no Info.plist at '${PLIST}'")
endif()

execute_process(COMMAND plutil -lint "${PLIST}"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "plutil -lint: ${out}")
endif()

set(failures "")

# The value at `key`, or the exit code's failure, in `out_var`.
function(plist_value key out_var found_var)
    execute_process(COMMAND /usr/libexec/PlistBuddy -c "Print ${key}" "${PLIST}"
        RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    set(${out_var} "${out}" PARENT_SCOPE)
    if(rc EQUAL 0)
        set(${found_var} TRUE PARENT_SCOPE)
    else()
        set(${found_var} FALSE PARENT_SCOPE)
    endif()
endfunction()

function(expect key expected)
    plist_value("${key}" value found)
    if(NOT found)
        set(failures "${failures}\n  ${key}: missing" PARENT_SCOPE)
    elseif(NOT value STREQUAL expected)
        set(failures "${failures}\n  ${key}: '${value}', expected '${expected}'" PARENT_SCOPE)
    endif()
endfunction()

function(expect_present key)
    plist_value("${key}" value found)
    if(NOT found)
        set(failures "${failures}\n  ${key}: missing" PARENT_SCOPE)
    endif()
endfunction()

function(expect_absent key why)
    plist_value("${key}" value found)
    if(found)
        set(failures "${failures}\n  ${key}: '${value}' (${why})" PARENT_SCOPE)
    endif()
endfunction()

# The service. NSPortName must be the executable's name: macOS reaches
# the provider through it.
expect(":CFBundleExecutable" "GitBolt")
expect(":NSServices:0:NSPortName" "GitBolt")
expect(":NSServices:0:NSMessage" "openInGitBolt")
expect(":NSServices:0:NSMenuItem:default" "Open in GitBolt")
expect(":NSServices:0:NSSendFileTypes:0" "public.folder")
expect_absent(":NSServices:0:NSSendFileTypes:1" "folders only")
# Without it the service is registered but not shown until turned on.
expect_present(":NSServices:0:NSRequiredContext")
expect_absent(":NSServices:1" "one service")

# Folders as documents: a Dock drop and `open -a GitBolt <folder>`.
# public.folder, never public.directory, which takes in .app bundles.
expect(":CFBundleDocumentTypes:0:LSItemContentTypes:0" "public.folder")
expect_absent(":CFBundleDocumentTypes:0:LSItemContentTypes:1" "folders only")
expect(":CFBundleDocumentTypes:0:CFBundleTypeRole" "Viewer")
# Never what opens a folder by default.
expect(":CFBundleDocumentTypes:0:LSHandlerRank" "Alternate")

if(failures)
    message(FATAL_ERROR "${PLIST}:${failures}")
endif()
message(STATUS "Finder integration keys OK: ${PLIST}")
