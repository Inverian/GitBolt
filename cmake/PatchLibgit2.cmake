# PATCH_COMMAND of the bundled libgit2 (cmake/Libgit2.cmake); runs in
# its freshly unpacked source directory. Idempotent, because FetchContent
# may run it again over an already patched tree. The first two changes
# only matter to MSVC, and are about libgit2 being a static library
# here; the third fixes a crash when a config file that changed can't
# be read again, on every platform, and the fourth the stale config
# that followed it. A version bump that moves the patched text fails
# here, loudly.

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

# get_entry() took any answer from a config backend other than "not
# found" for an entry, an error too: it then handed back the
# uninitialized entry pointer, and the callers that ignore errors
# (git_config__get_int_force and the other *_force lookups, such as
# status's rename limit) read it and called its free function through
# it, a jump to garbage. A backend errs when it rereads a config file
# that changed and the read fails: on Windows, while another process
# or handle is replacing .git/config (`git push -u`, `git config`, a
# remote added). test_git_service crashed so on Windows CI, at address
# 0, in about 1 in 35 runs of its remote-op tests (a status refresh
# reading the rename limit as the test rewrote .git/config through
# another handle). Unfixed in libgit2's main branch as of 2026-10-13;
# drop this once a release sets *out on success only.
gitbolt_patch(src/libgit2/config.c
    "\t\tif (res != GIT_ENOTFOUND) {\n\t\t\t*out = &be->entry;\n\t\t\tbreak;\n\t\t}"
    "\t\tif (res != GIT_ENOTFOUND) {\n\t\t\tif (res == 0)\n\t\t\t\t*out = &be->entry;\n\t\t\tbreak;\n\t\t}"
    "GIT_ENOTFOUND\\)[ \t\n]*{[ \t\n]*\\*out = &be->entry")

# A failed reread of a config file that changed kept the old entries,
# which is right, but also the new file's stamp, which
# config_file_is_modified() had already taken: every later lookup saw
# the stamp unchanged and the handle kept the old config until the file
# changed again (a remote added with git, then not listed). Forget the
# stamp when a refresh fails, so the next lookup reads the file again:
# a zeroed stamp matches no file, and the checksum then tells whether
# the content changed. Unfixed in libgit2's main branch as of
# 2026-10-13.
gitbolt_patch(src/libgit2/config_file.c
    "\tgit_config_list_free(config_list);\n\n\treturn (error == GIT_ENOTFOUND) ? 0 : error;"
    "\tgit_config_list_free(config_list);\n\tif (error < 0)\n\t\tgit_futils_filestamp_set(&b->file.stamp, NULL);\n\n\treturn (error == GIT_ENOTFOUND) ? 0 : error;"
    "git_config_list_free\\(config_list\\);[ \t\n]*return \\(error == GIT_ENOTFOUND\\)")
