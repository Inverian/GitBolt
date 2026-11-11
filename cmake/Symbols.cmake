# Debug info for the symbol files each release publishes (tools/symbolize.py
# reads crash reports with them). GITBOLT_RELEASE_SYMBOLS, which CI sets,
# adds line tables to Release builds at the same optimisation; the
# gitbolt_symbols target (cmake/Packaging.cmake) splits and zips them.
# Directory options, so the bundled libgit2 (include(Libgit2), after this)
# gets them too.

option(GITBOLT_RELEASE_SYMBOLS
    "Release builds with line tables, for the symbol files releases publish (CI sets it)"
    OFF)

if(GITBOLT_RELEASE_SYMBOLS)
    if(MSVC)
        # /Z7 keeps it in the .obj files: no shared compile PDB under Ninja.
        # MSVC has no line-tables-only mode. Not for rc.exe.
        add_compile_options("$<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:C,CXX>>:/Z7>")
    else()
        # Line tables only, on GCC and Clang; the code doesn't change.
        add_compile_options("$<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:C,CXX,OBJCXX>>:-g1>")
    endif()
endif()

# Every Linux binary carries a GNU build ID, whatever the linker's default:
# crash reports name it, and it ties a report to its symbol file.
if(UNIX AND NOT APPLE)
    add_link_options(LINKER:--build-id=sha1)
endif()
