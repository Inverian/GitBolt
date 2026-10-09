# PATCH_COMMAND of the bundled libgit2 (cmake/Libgit2.cmake); runs in
# its freshly unpacked source directory. Idempotent, because FetchContent
# may run it again over an already patched tree. Both changes only
# matter to MSVC, and both are about libgit2 being a static library here.
# A version bump that moves the patched text fails here, loudly.

# Replaces <from> with <to> in <file>, then fails if <leftover> (a regex)
# still matches: the upstream text changed and this patch needs updating.
function(gitbolt_patch file from to leftover)
    file(READ ${file} _text)
    string(REPLACE "${from}" "${to}" _patched "${_text}")
    if(_patched MATCHES "${leftover}")
        message(FATAL_ERROR "${file} no longer matches ${CMAKE_CURRENT_LIST_FILE}; update it")
    endif()
    if(NOT _patched STREQUAL _text)
        file(WRITE ${file} "${_patched}")
    endif()
endfunction()

# libgit2 hard-codes /GL (whole-program optimization) into its Release,
# RelWithDebInfo and MinSizeRel C flags. Inside a static library that
# turns every link against it into an /LTCG link ("restarting link with
# /LTCG"), redoing libgit2's code generation once per executable. vcpkg
# drops it for the same reason
# (ports/libgit2/no-static-whole-program-optimization.diff).
gitbolt_patch(cmake/DefaultCFlags.cmake
    " /GL /Gy" " /Gy"
    "set\\(CMAKE_C_FLAGS_[A-Z]+ \"[^\"]*/GL[ \"]")

# 1.9 declares every API function __declspec(dllexport) under MSVC, even
# in a static build, so each executable linking it would export all of
# libgit2 and get an import library of its own. libgit2's main branch
# gates this on GIT_STATIC; once a release has that, define GIT_STATIC
# on gitbolt_libgit2 instead.
gitbolt_patch(include/git2/common.h
    "# define GIT_EXTERN(type) __declspec(dllexport) type __cdecl"
    "# define GIT_EXTERN(type) type __cdecl"
    "define GIT_EXTERN\\(type\\) __declspec\\(dllexport\\)")
