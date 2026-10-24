# PATCH_COMMAND of the bundled libgit2 (cmake/Libgit2.cmake); runs in
# its freshly unpacked source directory. Idempotent, because FetchContent
# may run it again over an already patched tree. After an edit of this
# script it runs the new version over the tree the old one patched, so a
# patch whose result changed fails there, finding neither its text nor
# its result: delete the build directory's _deps/libgit2-* then. The
# first two changes only matter to MSVC, and are about libgit2 being a
# static library here; the third fixes a crash when a config file that
# changed can't be read again, on every platform, and the fourth the
# stale config that followed it. Then come crashes on config lines with
# no value, limits on what a repository's content can make libgit2 do (a
# pattern that never finishes matching, a tree nested too deep,
# attribute macros copied into every rule that names them), and last,
# two of libgit2's own fixes from its main branch, for reads of an
# object cut short that never ended. A version bump that moves the
# patched text fails here, loudly.

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

# A .gitattributes or .gitignore pattern with many stars and a literal
# that never comes (`*a*a*a*a*a*a*a*a*a*a*a*a*Q`) made wildmatch, which
# matches it against each path, backtrack exponentially: against a name
# of forty a's it never returned. The pattern is committed, so a clone
# brings it, and selecting a commit whose diff has such a path (the diff
# driver is an attribute) froze GitBolt for good at full CPU; with the
# path checked out, so did status, on opening. git's own wildmatch hangs
# on it too. When the literal after a star wasn't in the rest of the
# path, the star said WM_NOMATCH, and every star before it went on to
# try all its other places, each as hopeless. Now it stops them all
# (WM_ABORT_ALL; up to a `**` when a slash ended the search, as the
# star's own loop does). So do the other hopeless retries of the kind: a
# trailing star with a slash still to come, a star-slash with none to
# come, and the try of a `**/` as no directories when its rest can't
# match further on either (in a chain of them, that doubled the work at
# each). Each of these now takes a step or so per character of the path,
# and of twelve million fuzzed patterns and paths, every one the old
# matcher finishes matches as before.
#
# As a backstop, a match has a budget of steps: a pattern character
# tried, a character of the path skipped, a character of a [class] or of
# a run of stars read. Spent, the pattern doesn't match. It counts
# characters, not calls: a budget of a million calls still let one match
# against a 65,000-character name take 17 s, and a run of thousands of
# stars went by for one step. The budget is 1000 steps, then 64 per
# character of pattern and path. wildmatch() says WM_NOMATCH for any
# failure: pathspec_match_one() tries its fallbacks on that value alone,
# and the matches that now stop early used to end in it.
#
# And at most 256 stars match at once, each a level of recursion: a
# .gitignore line of 5,000 `**/` ran a worker thread computing status out
# of stack, a crash on opening.
gitbolt_patch(src/util/wildmatch.c
    "/* Match pattern \"p\" against \"text\" */\nstatic int dowild(const uchar *p, const uchar *text, unsigned int flags)\n{\n\tuchar p_ch;\n\tconst uchar *pattern = p;\n\n\tfor ( ; (p_ch = *p) != '\\0'; text++, p++) {\n\t\tint matched, match_slash, negated;\n\t\tuchar t_ch, prev_ch;\n"
    "/*\n * GitBolt: how many of a pattern's stars may be matching at once, each\n * a level of recursion. Thousands of <star star slash> in a pattern ran\n * a thread out of stack; no real pattern comes near this.\n */\n#define WM_MAX_DEPTH 256\n\n/* Match pattern \"p\" against \"text\" */\nstatic int dowild(const uchar *p, const uchar *text, unsigned int flags, long *budget, unsigned int depth)\n{\n\tuchar p_ch;\n\tconst uchar *pattern = p;\n\n\tif (depth > WM_MAX_DEPTH)\n\t\treturn WM_ABORT_ALL;\n\n\tfor ( ; (p_ch = *p) != '\\0'; text++, p++) {\n\t\tint matched, match_slash, negated;\n\t\tuchar t_ch, prev_ch;\n\t\t/* GitBolt: a match has a budget of steps (see wildmatch()),\n\t\t * spent here and in every loop that skips over the path or the\n\t\t * pattern, in case a pattern still finds a way to take long.\n\t\t * Spent, the pattern doesn't match. */\n\t\tif (--*budget <= 0)\n\t\t\treturn WM_ABORT_ALL;\n"
    "static int dowild\\(const uchar \\*p, const uchar \\*text, unsigned int flags\\)")
gitbolt_patch(src/util/wildmatch.c
    "\t\t\t\tconst uchar *prev_p = p - 2;\n\t\t\t\twhile (*++p == '*') {}\n\t\t\t\tif (!(flags & WM_PATHNAME))\n"
    "\t\t\t\tconst uchar *prev_p = p - 2;\n\t\t\t\twhile (*++p == '*') {}\n\t\t\t\t/* GitBolt: and a step for each star of the run after\n\t\t\t\t * the first: each try of the star skipped thousands of\n\t\t\t\t * them for one. */\n\t\t\t\tif ((*budget -= (long)(p - prev_p - 2)) <= 0)\n\t\t\t\t\treturn WM_ABORT_ALL;\n\t\t\t\tif (!(flags & WM_PATHNAME))\n"
    "while \\(\\*\\+\\+p == '\\*'\\) {}\n\t+if \\(!\\(flags & WM_PATHNAME\\)\\)")
gitbolt_patch(src/util/wildmatch.c
    "\t\t\t\t\tif (p[0] == '/' &&\n\t\t\t\t\t    dowild(p + 1, text, flags) == WM_MATCH)\n\t\t\t\t\t\treturn WM_MATCH;\n"
    "\t\t\t\t\t/*\n\t\t\t\t\t * GitBolt: and if the rest can't match here or\n\t\t\t\t\t * anywhere further on (WM_ABORT_ALL), it can't\n\t\t\t\t\t * after a slash further on either: stop. Trying\n\t\t\t\t\t * that anyway doubled the work at each of a\n\t\t\t\t\t * chain of these.\n\t\t\t\t\t */\n\t\t\t\t\tif (p[0] == '/' &&\n\t\t\t\t\t    ((matched = dowild(p + 1, text, flags, budget, depth + 1)) == WM_MATCH ||\n\t\t\t\t\t     matched == WM_ABORT_ALL))\n\t\t\t\t\t\treturn matched;\n"
    "dowild\\(p \\+ 1, text, flags\\)")
gitbolt_patch(src/util/wildmatch.c
    "\t\t\t\tif (!match_slash) {\n\t\t\t\t\tif (strchr((char*)text, '/') != NULL)\n\t\t\t\t\t\treturn WM_NOMATCH;\n\t\t\t\t}\n"
    "\t\t\t\tif (!match_slash) {\n\t\t\t\t\t/* GitBolt: and no later start of it or of the\n\t\t\t\t\t * stars before it, short of a '**', gets past\n\t\t\t\t\t * that slash (WM_ABORT_TO_STARSTAR). */\n\t\t\t\t\tconst char *slash = strchr((char*)text, '/');\n\t\t\t\t\tif (slash != NULL) {\n\t\t\t\t\t\t*budget -= (long)(slash - (const char*)text);\n\t\t\t\t\t\treturn WM_ABORT_TO_STARSTAR;\n\t\t\t\t\t}\n\t\t\t\t}\n"
    "if \\(strchr\\(\\(char\\*\\)text, '/'\\) != NULL\\)")
gitbolt_patch(src/util/wildmatch.c
    "\t\t\t\tconst char *slash = strchr((char*)text, '/');\n\t\t\t\tif (!slash)\n\t\t\t\t\treturn WM_NOMATCH;\n\t\t\t\ttext = (const uchar*)slash;\n"
    "\t\t\t\tconst char *slash = strchr((char*)text, '/');\n\t\t\t\tif (!slash) {\n\t\t\t\t\t/* GitBolt: nor after any later start. */\n\t\t\t\t\t*budget -= (long)strlen((char*)text);\n\t\t\t\t\treturn WM_ABORT_ALL;\n\t\t\t\t}\n\t\t\t\t*budget -= (long)(slash - (const char*)text);\n\t\t\t\ttext = (const uchar*)slash;\n"
    "if \\(!slash\\)\n\t+return WM_NOMATCH;")
gitbolt_patch(src/util/wildmatch.c
    "\t\t\t\t\t\tif (t_ch == p_ch)\n\t\t\t\t\t\t\tbreak;\n\t\t\t\t\t\ttext++;\n\t\t\t\t\t}\n\t\t\t\t\tif (t_ch != p_ch)\n\t\t\t\t\t\treturn WM_NOMATCH;\n\t\t\t\t}\n\t\t\t\tif ((matched = dowild(p, text, flags)) != WM_NOMATCH) {"
    "\t\t\t\t\t\tif (t_ch == p_ch)\n\t\t\t\t\t\t\tbreak;\n\t\t\t\t\t\tif (--*budget <= 0)\n\t\t\t\t\t\t\treturn WM_ABORT_ALL;\n\t\t\t\t\t\ttext++;\n\t\t\t\t\t}\n\t\t\t\t\t/*\n\t\t\t\t\t * GitBolt: the literal isn't in the rest of the\n\t\t\t\t\t * path, so no later start of this star or of the\n\t\t\t\t\t * ones before it finds it either (WM_ABORT_ALL);\n\t\t\t\t\t * when a slash stopped the search, a '**' further\n\t\t\t\t\t * out may still get past it (WM_ABORT_TO_STARSTAR).\n\t\t\t\t\t * WM_NOMATCH had each star before this one try all\n\t\t\t\t\t * its other places, in vain: a dozen stars and a\n\t\t\t\t\t * literal that never comes took forever.\n\t\t\t\t\t */\n\t\t\t\t\tif (t_ch != p_ch)\n\t\t\t\t\t\treturn t_ch ? WM_ABORT_TO_STARSTAR : WM_ABORT_ALL;\n\t\t\t\t}\n\t\t\t\tif ((matched = dowild(p, text, flags, budget, depth + 1)) != WM_NOMATCH) {"
    "matched = dowild\\(p, text, flags\\)\\)")
gitbolt_patch(src/util/wildmatch.c
    "\t\t\tdo {\n\t\t\t\tif (!p_ch)\n\t\t\t\t\treturn WM_ABORT_ALL;\n"
    "\t\t\tdo {\n\t\t\t\tif (!p_ch || --*budget <= 0)\n\t\t\t\t\treturn WM_ABORT_ALL;\n"
    "do {\n\t+if \\(!p_ch\\)\n")
gitbolt_patch(src/util/wildmatch.c
    "p++) {} /*SHARED ITERATOR*/\n\t\t\t\t\tif (!p_ch)\n\t\t\t\t\t\treturn WM_ABORT_ALL;\n"
    "p++) {} /*SHARED ITERATOR*/\n\t\t\t\t\tif (!p_ch || (*budget -= (long)(p - s)) <= 0)\n\t\t\t\t\t\treturn WM_ABORT_ALL;\n"
    "/\\*SHARED ITERATOR\\*/\n\t+if \\(!p_ch\\)\n")
gitbolt_patch(src/util/wildmatch.c
    "int wildmatch(const char *pattern, const char *text, unsigned int flags)\n{\n\treturn dowild((const uchar*)pattern, (const uchar*)text, flags);\n}"
    "int wildmatch(const char *pattern, const char *text, unsigned int flags)\n{\n\t/* GitBolt: an ordinary pattern takes one to three steps per\n\t * character of the path. A match gets 1000, and if they run out,\n\t * starts over with 64 per character of the pattern and the path. */\n\tlong budget = 1000;\n\tint error = dowild((const uchar*)pattern, (const uchar*)text, flags, &budget, 0);\n\n\tif (error == WM_ABORT_ALL && budget <= 0) {\n\t\tsize_t len = strlen(pattern) + strlen(text);\n\t\tbudget = 1000 + 64 * (long)(len < 1000000 ? len : 1000000);\n\t\terror = dowild((const uchar*)pattern, (const uchar*)text, flags, &budget, 0);\n\t}\n\t/*\n\t * GitBolt: callers ask only whether it matched, but\n\t * pathspec_match_one() tries its fallbacks (such as a file whose\n\t * name starts with '!') on WM_NOMATCH alone, and matches that used\n\t * to end in it now end in an abort above. So any failure says it.\n\t */\n\treturn error == WM_MATCH ? WM_MATCH : WM_NOMATCH;\n}"
    "return dowild\\(\\(const uchar\\*\\)pattern, \\(const uchar\\*\\)text, flags\\);")

# A tree nested thousands of levels deep, one directory in each (a few
# kilobytes of objects, which git fsck accepts and a clone delivers).
# libgit2's tree iterator keeps the path of every directory it is in, so
# what it held grew as the square of the depth: status, which every open
# runs, took 0.5 GB at a depth of 20,000, 10 GB at 100,000, and was
# killed for want of memory at 500,000; a commit's diff the same, on the
# GUI thread. git_tree_walk (the File Tree) recursed once per level
# until the stack ran out. Both now stop with "tree too deep" past 2048
# levels, git's own default (core.maxTreeDepth), or at a directory whose
# path is longer than GIT_PATH_MAX (4096 bytes, the longest path libgit2
# itself handles on disk), as long names would otherwise make even that
# depth dear. A tree_walk level takes 128 bytes of stack (112 at -O0),
# so 2048 of them, git_index_read_tree's included, take about 260 KB of
# a macOS worker thread's 512 KB. The work tree's iterator already stops
# at 100 levels.
gitbolt_patch(src/libgit2/tree.h
    "GIT_INLINE(bool) git_tree_entry__is_tree(const struct git_tree_entry *e)\n{\n\treturn (S_ISDIR(e->attr) && !S_ISGITLINK(e->attr));\n}\n\nvoid git_tree__free(void *tree);"
    "GIT_INLINE(bool) git_tree_entry__is_tree(const struct git_tree_entry *e)\n{\n\treturn (S_ISDIR(e->attr) && !S_ISGITLINK(e->attr));\n}\n\n/*\n * GitBolt: how deep the tree iterator and git_tree_walk() go. A tree\n * nested thousands of levels deep costs a few kilobytes of objects; the\n * iterator, which keeps the path of every directory it is in, then held\n * the square of the depth (gigabytes), and git_tree_walk() recursed\n * until the stack ran out. git stops at 2048 levels too (its\n * core.maxTreeDepth). A directory's path is held to GIT_PATH_MAX, or\n * long names would make even that depth dear.\n */\n#define GIT_TREE_MAX_DEPTH 2048\n\n/*\n * Whether a walk among the entries of a tree `depth` levels down (1 for\n * the root tree's) may go into one of them, a tree whose path (with its\n * trailing slash) is `path_len` bytes long.\n */\nGIT_INLINE(int) git_tree__descend_ok(size_t depth, size_t path_len)\n{\n\tif (depth >= GIT_TREE_MAX_DEPTH) {\n\t\tgit_error_set(GIT_ERROR_TREE,\n\t\t\t\"tree too deep: more than %d levels\", GIT_TREE_MAX_DEPTH);\n\t\treturn GIT_EINVALID;\n\t}\n\tif (path_len > GIT_PATH_MAX) {\n\t\tgit_error_set(GIT_ERROR_TREE,\n\t\t\t\"tree too deep: a path of more than %d bytes\", GIT_PATH_MAX);\n\t\treturn GIT_EINVALID;\n\t}\n\treturn 0;\n}\n\nvoid git_tree__free(void *tree);"
    "S_ISGITLINK\\(e->attr\\)\\);\n}\n\nvoid git_tree__free")
gitbolt_patch(src/libgit2/iterator.c
    "\tgit_tree *tree = NULL;\n\tint error;\n\n\tif ((error = git_tree_lookup(&tree,\n\t\t\titer->base.repo, &entry->tree_entry->oid)) < 0 ||\n\t\t(error = tree_iterator_frame_init(iter, tree, entry)) < 0)"
    "\tgit_tree *tree = NULL;\n\tint error;\n\n\t/* GitBolt: no deeper than GIT_TREE_MAX_DEPTH (see tree.h). */\n\tif ((error = git_tree__descend_ok(iter->frames.size,\n\t\t\t(entry->parent_path ? strlen(entry->parent_path) : 0) +\n\t\t\tentry->tree_entry->filename_len + 1)) < 0)\n\t\treturn error;\n\n\tif ((error = git_tree_lookup(&tree,\n\t\t\titer->base.repo, &entry->tree_entry->oid)) < 0 ||\n\t\t(error = tree_iterator_frame_init(iter, tree, entry)) < 0)"
    "int error;\n\n\tif \\(\\(error = git_tree_lookup\\(&tree,[ \t\n]*iter->base\\.repo, &entry->tree_entry->oid\\)\\) < 0 \\|\\|")
gitbolt_patch(src/libgit2/tree.c
    "\tgit_str *path,\n\tvoid *payload,\n\tbool preorder)\n{\n\tint error = 0;\n\tsize_t i;\n\tconst git_tree_entry *entry;\n"
    "\tgit_str *path,\n\tvoid *payload,\n\tbool preorder,\n\tsize_t depth)\n{\n\tint error = 0;\n\tsize_t i;\n\tconst git_tree_entry *entry;\n"
    "void \\*payload,\n\tbool preorder\\)\n{\n\tint error = 0;")
gitbolt_patch(src/libgit2/tree.c
    "\t\t\tsize_t path_len = git_str_len(path);\n\n\t\t\terror = git_tree_lookup(&subtree, tree->object.repo, &entry->oid);\n\t\t\tif (error < 0)\n\t\t\t\tbreak;\n\n\t\t\t/* append the next entry to the path */\n\t\t\tgit_str_puts(path, entry->filename);\n\t\t\tgit_str_putc(path, '/');\n\n\t\t\tif (git_str_oom(path))\n\t\t\t\terror = -1;\n\t\t\telse\n\t\t\t\terror = tree_walk(subtree, callback, path, payload, preorder);\n"
    "\t\t\tsize_t path_len = git_str_len(path);\n\n\t\t\t/* GitBolt: no deeper than GIT_TREE_MAX_DEPTH (see tree.h). */\n\t\t\terror = git_tree__descend_ok(depth,\n\t\t\t\tpath_len + entry->filename_len + 1);\n\t\t\tif (error < 0)\n\t\t\t\tbreak;\n\n\t\t\terror = git_tree_lookup(&subtree, tree->object.repo, &entry->oid);\n\t\t\tif (error < 0)\n\t\t\t\tbreak;\n\n\t\t\t/* append the next entry to the path */\n\t\t\tgit_str_puts(path, entry->filename);\n\t\t\tgit_str_putc(path, '/');\n\n\t\t\tif (git_str_oom(path))\n\t\t\t\terror = -1;\n\t\t\telse\n\t\t\t\terror = tree_walk(subtree, callback, path, payload, preorder, depth + 1);\n"
    "error = tree_walk\\(subtree, callback, path, payload, preorder\\);")
gitbolt_patch(src/libgit2/tree.c
    "\terror = tree_walk(\n\t\ttree, callback, &root_path, payload, (mode == GIT_TREEWALK_PRE));"
    "\terror = tree_walk(\n\t\ttree, callback, &root_path, payload, (mode == GIT_TREEWALK_PRE), 1);"
    "&root_path, payload, \\(mode == GIT_TREEWALK_PRE\\)\\);")

# A .gitattributes macro (`[attr]m a b c`, in the root one) was copied
# into every rule that named it as libgit2 read the file; git expands a
# macro only when it looks an attribute up. So the memory went as the
# names in the macro times the rules naming it: a macro of 8,000 names
# named by 8,000 rules (a 110 KB file) took a gigabyte, and twice the
# file four times that. A macro can also be built up from the one before
# it, a short line at a time, which git reads without a word. The file
# is committed, so a clone brings it, and selecting a commit with a
# change (the diff driver is an attribute) read it; the memory stayed
# taken as long as the repository was open. Now a file's rules take at
# most 1024 assignments plus one for each byte of the file, their
# macros' included: what the macros expand to grows with the file, as
# the rest of what libgit2 keeps of it does (a .gitattributes in each of
# many folders, each naming the macro, gets no more than its size allows
# either). A name written out takes two bytes at least, a letter and a
# space, so that is twice what a file without macros can reach, and
# real files stay far below it (`*.png binary` takes 4 in 13 bytes).
# At four a byte, short lines that each named 31 macros of 32 names held
# 110 MB for each MB of the file, nearly four times what the names
# written out cost; now 30, as they do. And one rule takes at most 1024,
# more than a line git reads (under 2048 bytes) can name: each goes into
# the rule's sorted list, and a line of thousands of names took time as
# their square (80,000: 0.4 s). A rule that would take more is skipped,
# as git skips a line too long. The 110 KB file now diffs in 5 ms and
# 8 MB.
gitbolt_patch(src/libgit2/attr_file.c
    "static bool parse_optimized_patterns(\n\tgit_attr_fnmatch *spec,\n\tgit_pool *pool,\n\tconst char *pattern);\n\nint git_attr_file__parse_buffer(\n\tgit_repository *repo, git_attr_file *attrs, const char *data, bool allow_macros)\n{\n\tconst char *scan = data, *context = NULL;\n\tgit_attr_rule *rule = NULL;\n\tint ignorecase = 0, error = 0;\n"
    "static bool parse_optimized_patterns(\n\tgit_attr_fnmatch *spec,\n\tgit_pool *pool,\n\tconst char *pattern);\n\n/*\n * GitBolt: libgit2 copies a macro into each rule that names it, so N\n * rules naming a macro of K names took N x K. A rule takes at most this\n * many assignments, its macros' included (a macro is a rule too): git\n * skips a line of 2048 bytes or more, and a shorter one names no more.\n * Each goes into the rule's sorted list, which took time as the square\n * of a long line's names.\n */\n#define GIT_ATTR_MAX_RULE_ASSIGNS 1024\n\nstatic int attr_assignment_parse(\n\tgit_repository *repo,\n\tgit_pool *pool,\n\tgit_vector *assigns,\n\tconst char **base,\n\tsize_t *budget);\n\nint git_attr_file__parse_buffer(\n\tgit_repository *repo, git_attr_file *attrs, const char *data, bool allow_macros)\n{\n\tconst char *scan = data, *context = NULL;\n\tgit_attr_rule *rule = NULL;\n\tint ignorecase = 0, error = 0;\n\t/* GitBolt: and the file's rules, all together, a rule's worth plus\n\t * one for each byte of the file. A name written out takes two bytes\n\t * at least, so macros can't make a file hold much more than its\n\t * names would. A rule past either limit is skipped. */\n\tsize_t budget = GIT_ATTR_MAX_RULE_ASSIGNS + strlen(data);\n"
    "int ignorecase = 0, error = 0;\n\n\t/\\* If subdir file path")
gitbolt_patch(src/libgit2/attr_file.c
    "(error = git_attr_assignment__parse(repo, &attrs->pool, &rule->assigns, &scan)) < 0)"
    "(error = attr_assignment_parse(repo, &attrs->pool, &rule->assigns, &scan, &budget)) < 0)"
    "git_attr_assignment__parse\\(repo, &attrs->pool, &rule->assigns, &scan\\)")
gitbolt_patch(src/libgit2/attr_file.c
    "int git_attr_assignment__parse(\n\tgit_repository *repo,\n\tgit_pool *pool,\n\tgit_vector *assigns,\n\tconst char **base)\n{\n\tint error;\n\tconst char *scan = *base;\n\tgit_attr_assignment *assign = NULL;\n"
    "int git_attr_assignment__parse(\n\tgit_repository *repo,\n\tgit_pool *pool,\n\tgit_vector *assigns,\n\tconst char **base)\n{\n\tsize_t budget = GIT_ATTR_MAX_RULE_ASSIGNS;\n\n\treturn attr_assignment_parse(repo, pool, assigns, base, &budget);\n}\n\n/*\n * GitBolt: takes n more assignments for a rule, from what the rule and\n * its file have left, or says one of them hasn't that many.\n */\nstatic bool attr_assignments_take(size_t *rule_left, size_t *budget, size_t n)\n{\n\tif (n > *rule_left || n > *budget)\n\t\treturn false;\n\t*rule_left -= n;\n\t*budget -= n;\n\treturn true;\n}\n\nstatic int attr_assignment_parse(\n\tgit_repository *repo,\n\tgit_pool *pool,\n\tgit_vector *assigns,\n\tconst char **base,\n\tsize_t *budget)\n{\n\tint error;\n\tconst char *scan = *base;\n\tgit_attr_assignment *assign = NULL;\n\tsize_t rule_left = GIT_ATTR_MAX_RULE_ASSIGNS;\n"
    "const char \\*\\*base\\)\n{\n\tint error;")
gitbolt_patch(src/libgit2/attr_file.c
    "\t\t\t\tgit_attr_assignment *massign;\n\n\t\t\t\tgit_vector_foreach(&macro->assigns, i, massign) {\n"
    "\t\t\t\tgit_attr_assignment *massign;\n\n\t\t\t\t/* GitBolt: a macro's assignments count as the rule's. */\n\t\t\t\tif (!attr_assignments_take(&rule_left, budget, macro->assigns.length))\n\t\t\t\t\tgoto skip;\n\n\t\t\t\tgit_vector_foreach(&macro->assigns, i, massign) {\n"
    "massign;\n\n\t+git_vector_foreach\\(&macro->assigns")
gitbolt_patch(src/libgit2/attr_file.c
    "\t\t/* insert allocated assign into vector */\n\t\terror = git_vector_insert_sorted(assigns, assign, &merge_assignments);\n"
    "\t\t/* insert allocated assign into vector */\n\t\tif (!attr_assignments_take(&rule_left, budget, 1))\n\t\t\tgoto skip;\n\t\terror = git_vector_insert_sorted(assigns, assign, &merge_assignments);\n"
    "/\\* insert allocated assign into vector \\*/\n\t+error = ")
gitbolt_patch(src/libgit2/attr_file.c
    "\t*base = git__next_line(scan);\n\n\treturn (assigns->length == 0) ? GIT_ENOTFOUND : 0;\n}\n"
    "\t*base = git__next_line(scan);\n\n\treturn (assigns->length == 0) ? GIT_ENOTFOUND : 0;\n\nskip:\n\t/* GitBolt: the rule took too many; it is skipped, as one with\n\t * none. What it took is the caller's to free with the rule. */\n\tgit_attr_assignment__free(assign);\n\t*base = git__next_line(scan);\n\treturn GIT_ENOTFOUND;\n}\n"
    "return \\(assigns->length == 0\\) \\? GIT_ENOTFOUND : 0;\n}")

# An object cut short: a loose object whose file ends inside its zlib
# stream, or a delta in a pack whose data runs out before its header
# does. inflate() then says Z_BUF_ERROR, which libgit2 takes for "give
# me more room" (zstream_seterr), and the loops reading on had no more
# input to give it: they asked again, forever, at full CPU. Reading the
# loose object (any lookup of it: a commit, a blob for a diff or the
# File view) or the packed delta's header (a diff's file sizes, an
# object's type check) never returned. A disk or sync error or an
# interrupted write can leave a loose object so, and a repository can
# be made so. Both reads now fail ("premature end of zlib stream",
# "truncated packfile"). And an error from the delta's stream no longer
# adds -1 to the header's length, which wrapped it and had the header
# read past its 16-byte buffer: a corrupt pack, not only a short one. libgit2's own fixes, from its main branch
# (5254f5dc, PR #7346; PR #7402), in v2.0.0-rc.1 but in no 1.9 release
# as of 2026-10-24. Their code as it is, comments included: a release
# that has them then finds these patches' result already there, and
# they do nothing. Drop them then.
gitbolt_patch(src/util/zstream.c
    "\t\tif (git_zstream_get_output_chunk(out, &out_written, zstream) < 0)\n\t\t\treturn -1;\n\n\t\tout_remain -= out_written;"
    "\t\tif (git_zstream_get_output_chunk(out, &out_written, zstream) < 0)\n\t\t\treturn -1;\n\n\t\t/*\n\t\t * with output space still available, a Z_BUF_ERROR that wrote\n\t\t * nothing could be the input is exhausted in the middle of the stream.\n\t\t * It is unlikely further input will arrive within this loop,\n\t\t * so we fail it.\n\t\t */\n\t\tif (!out_written && zstream->zerr == Z_BUF_ERROR) {\n\t\t\tgit_error_set(GIT_ERROR_ZLIB, \"premature end of zlib stream\");\n\t\t\treturn -1;\n\t\t}\n\n\t\tout_remain -= out_written;"
    "&out_written, zstream\\) < 0\\)\n\t+return -1;\n\n\t+out_remain -= out_written;")
gitbolt_patch(src/libgit2/delta.c
    "\twhile (len < buffer_len) {\n\t\tread = git_packfile_stream_read(stream, &buffer[len], buffer_len - len);\n\n\t\tif (read == 0)\n\t\t\tbreak;\n\n\t\tif (read == GIT_EBUFS)\n\t\t\tcontinue;\n\n\t\tlen += read;\n\t}"
    "\twhile (len < buffer_len) {\n\t\toff64_t curpos = stream->curpos;\n\n\t\tread = git_packfile_stream_read(stream, &buffer[len], buffer_len - len);\n\n\t\tif (read == 0)\n\t\t\tbreak;\n\n\t\tif (read == GIT_EBUFS) {\n\t\t\t/*\n\t\t\t * The stream made no progress at all: inflate()\n\t\t\t * stalled on truncated input (Z_BUF_ERROR) and the\n\t\t\t * pack has no more data to offer (curpos is stuck\n\t\t\t * past the end of the packfile).  Retrying would\n\t\t\t * loop forever, so fail instead of hanging.\n\t\t\t */\n\t\t\tif (stream->curpos == curpos) {\n\t\t\t\tgit_error_set(GIT_ERROR_ODB, \"truncated packfile\");\n\t\t\t\treturn -1;\n\t\t\t}\n\t\t\tcontinue;\n\t\t}\n\n\t\tif (read < 0)\n\t\t\treturn -1;\n\n\t\tlen += read;\n\t}"
    "if \\(read == GIT_EBUFS\\)\n\t+continue;")
