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
# A submodule that leads back out of its superproject: `path = /`
# (joined onto the work tree, it names the work tree), a path through a
# committed symlink to `.` or `..`, one level down or several, or a
# repository whose core.worktree (or a worktree's gitdir file) puts its
# work tree back at the superproject's. Status opened a repository it
# was already reading as a submodule of its own, diffed it, met the
# submodule again, and so on until the stack ran out: a clone of such a
# repository crashed GitBolt on opening. Now a submodule is opened only
# if its folder lies strictly inside the superproject's and its path
# goes through no symlink (git's own rule; Windows' realpath doesn't
# resolve symlinks, so this is what catches them there), and the
# repository opened must not have its work tree at the superproject's
# or above it. Each level a submodule path opens then lies deeper than
# the one before. A refused submodule is taken as not checked out.
gitbolt_patch(src/libgit2/submodule.c
    "\tif (bare)\n\t\tflags |= GIT_REPOSITORY_OPEN_BARE;\n\n\terror = git_repository_open_ext(subrepo, path.ptr, flags, wd);\n\n\t/* if we opened"
    "\tif (bare)\n\t\tflags |= GIT_REPOSITORY_OPEN_BARE;\n\n\t/* GitBolt: a submodule is opened only strictly inside its\n\t * superproject, and never through a symlink (git's own rule). One that\n\t * is the superproject (`path = /`), or leads out of it (a symlink to\n\t * \".\" or \"..\"), opened a repository status was already reading, and\n\t * status recursed until the stack ran out. */\n\t{\n\t\tgit_str dir = GIT_STR_INIT, real = GIT_STR_INIT, realwd = GIT_STR_INIT;\n\t\tconst char *p = sm->path;\n\t\tint inside = 1, oom = git_str_puts(&dir, wd) < 0;\n\n\t\twhile (!oom && *p && inside) {\n\t\t\tconst char *slash = strchr(p, '/');\n\t\t\tsize_t len = slash ? (size_t)(slash - p) : strlen(p);\n\t\t\tstruct stat st;\n\n\t\t\tif (len) {\n\t\t\t\tif ((dir.size && dir.ptr[dir.size - 1] != '/' &&\n\t\t\t\t     git_str_putc(&dir, '/') < 0) ||\n\t\t\t\t    git_str_put(&dir, p, len) < 0) {\n\t\t\t\t\toom = 1;\n\t\t\t\t\tbreak;\n\t\t\t\t}\n\t\t\t\tif (p_lstat(dir.ptr, &st) == 0 && S_ISLNK(st.st_mode))\n\t\t\t\t\tinside = 0;\n\t\t\t}\n\t\t\tif (!slash)\n\t\t\t\tbreak;\n\t\t\tp = slash + 1;\n\t\t}\n\t\tif (!oom && inside &&\n\t\t    git_fs_path_prettify_dir(&real, dir.ptr, NULL) == 0 &&\n\t\t    git_fs_path_prettify_dir(&realwd, wd, NULL) == 0)\n\t\t\tinside = !git__prefixcmp(real.ptr, realwd.ptr) && real.size > realwd.size;\n\t\tif (git_str_oom(&real) || git_str_oom(&realwd))\n\t\t\toom = 1;\n\t\tif (!oom)\n\t\t\tgit_error_clear();\n\t\tgit_str_dispose(&dir);\n\t\tgit_str_dispose(&real);\n\t\tgit_str_dispose(&realwd);\n\t\tif (oom) {\n\t\t\tgit_str_dispose(&path);\n\t\t\treturn -1;\n\t\t}\n\t\tif (!inside) {\n\t\t\tgit_str_dispose(&path);\n\t\t\tsm->flags |= GIT_SUBMODULE_STATUS__WD_SCANNED;\n\t\t\tgit_error_set(GIT_ERROR_SUBMODULE,\n\t\t\t\t\"submodule '%s' is not inside its superproject\", sm->name);\n\t\t\treturn GIT_EINVALID;\n\t\t}\n\t}\n\n\terror = git_repository_open_ext(subrepo, path.ptr, flags, wd);\n\n\t/* GitBolt: nor may the repository opened have its work tree at the\n\t * superproject's, or above it (core.worktree, or a worktree's gitdir\n\t * file, can put it there): status would read the superproject again. */\n\tif (!error && !bare && git_repository_workdir(*subrepo)) {\n\t\tgit_str real = GIT_STR_INIT, realwd = GIT_STR_INIT;\n\t\tint loops = 0, oom = 0;\n\n\t\tif (git_fs_path_prettify_dir(&real, git_repository_workdir(*subrepo), NULL) == 0 &&\n\t\t    git_fs_path_prettify_dir(&realwd, wd, NULL) == 0)\n\t\t\tloops = !git__prefixcmp(realwd.ptr, real.ptr);\n\t\telse if (git_str_oom(&real) || git_str_oom(&realwd))\n\t\t\toom = 1;\n\t\telse\n\t\t\tgit_error_clear();\n\t\tgit_str_dispose(&real);\n\t\tgit_str_dispose(&realwd);\n\t\tif (oom || loops) {\n\t\t\tgit_repository_free(*subrepo);\n\t\t\t*subrepo = NULL;\n\t\t\tgit_str_dispose(&path);\n\t\t\tif (oom)\n\t\t\t\treturn -1;\n\t\t\tsm->flags |= GIT_SUBMODULE_STATUS__WD_SCANNED;\n\t\t\tgit_error_set(GIT_ERROR_SUBMODULE,\n\t\t\t\t\"submodule '%s' is not inside its superproject\", sm->name);\n\t\t\treturn GIT_EINVALID;\n\t\t}\n\t}\n\n\t/* if we opened"
    "OPEN_BARE;\n\n\terror = git_repository_open_ext\\(subrepo")
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
# Rewriting a config file to remove or replace the values that match a
# pattern (a remote's refspecs when it is added, every entry of a
# branch's or a remote's section when it is deleted) matched the pattern
# against each line's value, and a line with no value has none:
# strlen(NULL). Deleting a branch with a `rebase` line, or removing a
# remote with a `prune` or `mirror` one (git booleans: no value is
# true), crashed GitBolt and left .git/config.lock behind, which then
# failed every config write, git's too. A line with no value matches as
# the empty value, as git_config_get_string() reads it.
gitbolt_patch(src/libgit2/config_file.c
    "\t\thas_matched = (git_regexp_match(write_data->preg, var_value) == 0);"
    "\t\thas_matched = (git_regexp_match(write_data->preg, var_value ? var_value : \"\") == 0);"
    "git_regexp_match\\(write_data->preg, var_value\\)")
# The same strlen(NULL) for a caller that passes a value pattern to
# git_config_get_multivar_foreach() or git_config_multivar_iterator_new()
# (GitBolt passes none).
gitbolt_patch(src/libgit2/config.c
    "\t\tif (git_regexp_match(&iter->regex, (*entry)->entry.value) == 0)"
    "\t\tif (git_regexp_match(&iter->regex, (*entry)->entry.value ? (*entry)->entry.value : \"\") == 0)"
    "git_regexp_match\\(&iter->regex, \\(\\*entry\\)->entry\\.value\\)")
# merge.conflictstyle with no value (in ~/.gitconfig: for every
# repository): every checkout crashed, switching branch, discarding a
# file's changes, stash save, apply and pop, a cherry-pick, adding a
# worktree. Now it fails as git does.
gitbolt_patch(src/libgit2/checkout.c
    "\t\telse if (error)\n\t\t\tgoto cleanup;\n\t\telse if (strcmp(conflict_style->value, \"merge\") == 0)"
    "\t\telse if (error)\n\t\t\tgoto cleanup;\n\t\telse if (!conflict_style->value) {\n\t\t\tgit_error_set(GIT_ERROR_CHECKOUT,\n\t\t\t\t\"missing value for 'merge.conflictstyle'\");\n\t\t\terror = -1;\n\t\t\tgit_config_entry_free(conflict_style);\n\t\t\tgoto cleanup;\n\t\t}\n\t\telse if (strcmp(conflict_style->value, \"merge\") == 0)"
    "goto cleanup;\n\t\telse if \\(strcmp\\(conflict_style->value, \"merge\"\\)")
# merge.default with no value: a cherry-pick (any libgit2 merge)
# crashed. Now it fails as git does. The similarity metric allocated
# above is the caller's to free only once this returns success.
gitbolt_patch(src/libgit2/merge.c
    "\t\tif (error == 0) {\n\t\t\topts->default_driver = git__strdup(entry->value);"
    "\t\tif (error == 0 && !entry->value) {\n\t\t\tgit_error_set(GIT_ERROR_MERGE,\n\t\t\t\t\"missing value for 'merge.default'\");\n\t\t\terror = GIT_EINVALID;\n\t\t\tif (!given || !given->metric) {\n\t\t\t\tgit__free(opts->metric);\n\t\t\t\topts->metric = NULL;\n\t\t\t}\n\t\t\tgoto done;\n\t\t} else if (error == 0) {\n\t\t\topts->default_driver = git__strdup(entry->value);"
    "\n\t\tif \\(error == 0\\) {\n\t\t\topts->default_driver = git__strdup\\(entry->value\\)")
# init.defaultBranch with no value: git_repository_is_empty() crashed
# (GitBolt doesn't call it). Taken as unset, as repository init does.
gitbolt_patch(src/libgit2/repository.c
    "\tif ((error = git_config_get_entry(&entry, config, \"init.defaultbranch\")) == 0 &&\n\t\t*entry->value) {"
    "\tif ((error = git_config_get_entry(&entry, config, \"init.defaultbranch\")) == 0 &&\n\t\tentry->value && *entry->value) {"
    "\"init\\.defaultbranch\"\\)\\) == 0 &&\n\t\t\\*entry->value\\)")
