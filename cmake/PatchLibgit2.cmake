# PATCH_COMMAND of the bundled libgit2 (cmake/Libgit2.cmake); runs in
# its freshly unpacked source directory. Idempotent, because FetchContent
# may run it again over an already patched tree. After an edit of this
# script it runs the new version over the tree the old one patched, so a
# patch whose result changed fails there, finding neither its text nor
# its result: delete the build directory's _deps/libgit2-* then. The
# first two changes only matter to MSVC, and are about libgit2 being a
# static library here; the third fixes a crash when a config file that
# changed can't be read again, on every platform, and the fourth the
# stale config that followed it. The rest stop crashes on config lines
# with no value. A version bump that moves the patched text fails here,
# loudly.

# Replaces <from> with <to> in <file>, then fails if <leftover> (a regex)
# still matches: the upstream text changed and this patch needs updating.
# Fails as well if <file> then lacks <to>: it had neither the text this
# patch replaces nor its result.
function(gitbolt_patch file from to leftover)
    file(READ ${file} _text)
    string(REPLACE "${from}" "${to}" _patched "${_text}")
    if(_patched MATCHES "${leftover}")
        message(FATAL_ERROR "${file} no longer matches ${CMAKE_CURRENT_LIST_FILE}; update it")
    endif()
    string(FIND "${_patched}" "${to}" _at)
    if(_at EQUAL -1)
        message(FATAL_ERROR "${file} has neither the text ${CMAKE_CURRENT_LIST_FILE} "
            "patches nor its result: unpack libgit2 again (for a build directory, "
            "delete its _deps/libgit2-* and configure), or update the script")
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
# which is right, but also the file's new stamp and, as it is hashed
# before it's parsed, its new checksum. The parse also reads the files
# it includes (include.path, includeIf), taking their stamps, and for
# onbranch reads HEAD, taking none (upstream, a branch switch alone
# isn't seen until a config file changes). When any of them couldn't be
# read, every later lookup found nothing changed, and the handle kept
# the old config until a file changed again (a remote added with git,
# then not listed). Forget the stamp and the checksum when a refresh
# fails: they then match no file and no content, so the next lookup
# reads everything again. Unfixed in libgit2's main branch as of
# 2026-10-13.
gitbolt_patch(src/libgit2/config_file.c
    "\tgit_config_list_free(config_list);\n\n\treturn (error == GIT_ENOTFOUND) ? 0 : error;"
    "\tgit_config_list_free(config_list);\n\tif (error < 0) {\n\t\tgit_futils_filestamp_set(&b->file.stamp, NULL);\n\t\tmemset(b->file.checksum, 0, sizeof(b->file.checksum));\n\t}\n\n\treturn (error == GIT_ENOTFOUND) ? 0 : error;"
    "git_config_list_free\\(config_list\\);[ \t\n]*return \\(error == GIT_ENOTFOUND\\)")

# A config line with a name and no "= value" (`path`, not `path = x`) is
# an entry whose value is NULL: git's "true" for a boolean. Where libgit2
# wants a string it used the NULL as one, and crashed (strchr, strcmp,
# strlen) where git refuses the config with "missing value for ...".
#
# .gitmodules is committed, so a repository can bring one: a `path` with
# no value crashed GitBolt on opening it (Repository::submodules, which
# every open runs). Skip such an entry, as one with no usable path.
gitbolt_patch(src/libgit2/submodule.c
    "\t\tconst char *fdot, *ldot;\n\n\t\tfdot = strchr(entry->name, '.');\n\t\tldot = strrchr(entry->name, '.');\n\n\t\tif (git_submodule_namemap_contains(names, entry->value)) {"
    "\t\tconst char *fdot, *ldot;\n\n\t\tif (!entry->value)\n\t\t\tcontinue;\n\n\t\tfdot = strchr(entry->name, '.');\n\t\tldot = strrchr(entry->name, '.');\n\n\t\tif (git_submodule_namemap_contains(names, entry->value)) {"
    "ldot;\n\n\t\tfdot = strchr\\(entry->name, '\\.'\\);\n\t\tldot = strrchr\\(entry->name, '\\.'\\);\n\n\t\tif \\(git_submodule_namemap_contains")
gitbolt_patch(src/libgit2/submodule.c
    "\tif (!strcmp(entry->value, data->path)) {"
    "\tif (entry->value && !strcmp(entry->value, data->path)) {"
    "if \\(!strcmp\\(entry->value, data->path\\)\\)")
# And reading the submodule's own config, libgit2 takes a `path` with no
# value as "" (git_config_get_string), as it does `path =`: the
# submodule's folder was the superproject's, and status, which every
# open runs, opened the superproject as its own submodule, over and
# over, until the stack ran out (a clone of such a repository: on
# opening). An empty path is ignored: the submodule keeps its name as
# its path, as when there is none. (Upstream crashes on `path =` too.)
gitbolt_patch(src/libgit2/submodule.c
    "\t\tin_config = 1;\n\t\t/* We would warn here if we had that API */\n\t\tif (!looks_like_command_line_option(value)) {"
    "\t\tin_config = 1;\n\t\t/* We would warn here if we had that API */\n\t\tif (*value && !looks_like_command_line_option(value)) {"
    "in_config = 1;\n\t\t/\\* We would warn here if we had that API \\*/\n\t\tif \\(!looks_like_command_line_option")
# url.<base>.insteadOf / pushInsteadOf with no value: every remote lookup
# (the Remote repositories list, Edit URL, Statistics) crashed.
gitbolt_patch(src/libgit2/remote.c
    "\t\tif (git__prefixcmp(url, entry->value))\n\t\t\tcontinue;"
    "\t\tif (!entry->value || git__prefixcmp(url, entry->value))\n\t\t\tcontinue;"
    "if \\(git__prefixcmp\\(url, entry->value\\)\\)")
# branch.<name>.remote with no value: renaming or removing any remote
# crashed.
gitbolt_patch(src/libgit2/remote.c
    "\tif (strcmp(entry->value, data->old_remote_name))\n\t\treturn 0;"
    "\tif (!entry->value || strcmp(entry->value, data->old_remote_name))\n\t\treturn 0;"
    "if \\(strcmp\\(entry->value, data->old_remote_name\\)\\)")
gitbolt_patch(src/libgit2/remote.c
    "\t\tif (strcmp(remote_name, entry->value))\n\t\t\tcontinue;"
    "\t\tif (!entry->value || strcmp(remote_name, entry->value))\n\t\t\tcontinue;"
    "if \\(strcmp\\(remote_name, entry->value\\)\\)")
# extensions.objectformat with no value: opening the repository, or any
# folder in it, crashed. Now it fails as git does.
gitbolt_patch(src/libgit2/repository.c
    "\tif ((repo->oid_type = git_oid_type_fromstr(entry->value)) == 0) {"
    "\tif (!entry->value) {\n\t\tgit_error_set(GIT_ERROR_REPOSITORY,\n\t\t\t\"missing value for 'extensions.objectformat'\");\n\t\terror = GIT_EINVALID;\n\t} else if ((repo->oid_type = git_oid_type_fromstr(entry->value)) == 0) {"
    "\n\tif \\(\\(repo->oid_type = git_oid_type_fromstr\\(entry->value\\)\\) == 0\\)")
