# The oldest system libgit2 GitBolt takes (cmake/Libgit2.cmake), and the
# oldest on the 1.8 series: the releases that fixed a submodule path that
# leads out of the work tree (CVE-2026-53584), a delta that allocates
# without bound (CVE-2026-53585) and a heap overflow in the bundled PCRE
# that a `:/<pattern>` revision reaches. Their other CVEs, and 1.9.7's,
# are in network transports GitBolt doesn't use. A file of its own, so
# tests/cmake/TestLibgit2Floor.cmake can try it.
set(GITBOLT_LIBGIT2_FLOOR 1.9.5)
set(GITBOLT_LIBGIT2_FLOOR_1_8 1.8.6)

function(gitbolt_libgit2_version_ok version out_var)
    if(version VERSION_GREATER_EQUAL ${GITBOLT_LIBGIT2_FLOOR}
       OR (version VERSION_GREATER_EQUAL ${GITBOLT_LIBGIT2_FLOOR_1_8} AND version VERSION_LESS 1.9))
        set(${out_var} TRUE PARENT_SCOPE)
    else()
        set(${out_var} FALSE PARENT_SCOPE)
    endif()
endfunction()
