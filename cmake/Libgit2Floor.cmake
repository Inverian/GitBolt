# The oldest system libgit2 GitBolt takes (cmake/Libgit2.cmake): one with
# libgit2's own fixes for what a repository can do to it. 1.9.5, and
# 1.8.6 on 1.8, fixed a submodule path that leads out of the work tree
# (CVE-2026-53584), a delta whose size header allocates without bound
# (CVE-2026-53585), and a heap overflow in the bundled PCRE that a
# `:/<pattern>` revision reaches (Go to commit takes one); 1.7.2 fixed
# git_index_add's heap corruption (CVE-2024-24577). Their other CVEs,
# and 1.9.7's, are in network transports GitBolt doesn't use. A file of
# its own, so tests/cmake/TestLibgit2Floor.cmake can try it.
function(gitbolt_libgit2_version_ok version out_var)
    if(version VERSION_GREATER_EQUAL 1.9.5
       OR (version VERSION_GREATER_EQUAL 1.8.6 AND version VERSION_LESS 1.9))
        set(${out_var} TRUE PARENT_SCOPE)
    else()
        set(${out_var} FALSE PARENT_SCOPE)
    endif()
endfunction()
