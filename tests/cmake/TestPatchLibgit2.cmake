# cmake -DPATCH_SCRIPT=<cmake/PatchLibgit2.cmake> -DWORK_DIR=<scratch> -P this
#
# Runs the bundled libgit2's patch script over a stand-in source tree:
# the lines it patches, as libgit2 1.9.7 has them. FetchContent runs it
# in a freshly unpacked tree, and runs an edited version of it again over
# the tree it already patched, not over a fresh copy.

foreach(_var PATCH_SCRIPT WORK_DIR)
    if(NOT DEFINED ${_var})
        message(FATAL_ERROR "pass -D${_var}=...")
    endif()
endforeach()

# The text each patched file has before and after.
set(_cflags_from
    "\tset(CMAKE_C_FLAGS_RELEASE \"/DNDEBUG /O2 /Oy /GL /Gy \${CRT_FLAG_RELEASE}\")\n\tset(CMAKE_C_FLAGS_RELWITHDEBINFO \"/DNDEBUG /Zi /O2 /Oy- /GL /Gy \${CRT_FLAG_RELEASE}\")\n\tset(CMAKE_C_FLAGS_MINSIZEREL \"/DNDEBUG /O1 /Oy /GL /Gy \${CRT_FLAG_RELEASE}\")\n")
set(_cflags_to
    "\tset(CMAKE_C_FLAGS_RELEASE \"/DNDEBUG /O2 /Oy /Gy \${CRT_FLAG_RELEASE}\")\n\tset(CMAKE_C_FLAGS_RELWITHDEBINFO \"/DNDEBUG /Zi /O2 /Oy- /Gy \${CRT_FLAG_RELEASE}\")\n\tset(CMAKE_C_FLAGS_MINSIZEREL \"/DNDEBUG /O1 /Oy /Gy \${CRT_FLAG_RELEASE}\")\n")
set(_common_from "# define GIT_EXTERN(type) __declspec(dllexport) type __cdecl\n")
set(_common_to "# define GIT_EXTERN(type) type __cdecl\n")
# Each file's text is in two parts where the 0.9.7 script patched one
# and a later version the other (_old and _new), so a tree the 0.9.7
# script patched can be put together (_old_to with _new_from).
set(_config_old_from
    "\t\tif (res != GIT_ENOTFOUND) {\n\t\t\t*out = &be->entry;\n\t\t\tbreak;\n\t\t}\n")
set(_config_old_to
    "\t\tif (res != GIT_ENOTFOUND) {\n\t\t\tif (res == 0)\n\t\t\t\t*out = &be->entry;\n\t\t\tbreak;\n\t\t}\n")
set(_config_new_from
    "\t\tif (git_regexp_match(&iter->regex, (*entry)->entry.value) == 0)\n\t\t\treturn 0;\n")
set(_config_new_to
    "\t\tif (git_regexp_match(&iter->regex, (*entry)->entry.value ? (*entry)->entry.value : \"\") == 0)\n\t\t\treturn 0;\n")
set(_config_file_old_from
    "out:\n\tgit_config_list_free(config_list);\n\n\treturn (error == GIT_ENOTFOUND) ? 0 : error;\n}\n")
set(_config_file_old_to
    "out:\n\tgit_config_list_free(config_list);\n\tif (error < 0) {\n\t\tgit_futils_filestamp_set(&b->file.stamp, NULL);\n\t\tmemset(b->file.checksum, 0, sizeof(b->file.checksum));\n\t}\n\n\treturn (error == GIT_ENOTFOUND) ? 0 : error;\n}\n")
set(_config_file_new_from
    "\tif (has_matched && write_data->preg != NULL)\n\t\thas_matched = (git_regexp_match(write_data->preg, var_value) == 0);\n")
set(_config_file_new_to
    "\tif (has_matched && write_data->preg != NULL)\n\t\thas_matched = (git_regexp_match(write_data->preg, var_value ? var_value : \"\") == 0);\n")
# The config-lines-with-no-value fixes.
set(_submodule_from
    "\twhile ((error = git_config_next(&entry, iter)) == 0) {\n\t\tconst char *fdot, *ldot;\n\n\t\tfdot = strchr(entry->name, '.');\n\t\tldot = strrchr(entry->name, '.');\n\n\t\tif (git_submodule_namemap_contains(names, entry->value)) {\n}\n\tif (!strcmp(entry->value, data->path)) {\n\tif ((error = get_value(&value, cfg, &key, sm->name, \"path\")) == 0) {\n\t\tin_config = 1;\n\t\t/* We would warn here if we had that API */\n\t\tif (!looks_like_command_line_option(value)) {\n\tif ((error = get_value(&value, cfg, &key, sm->name, \"url\")) == 0) {\n\t\t/* We would warn here if we had that API */\n\t\tif (!looks_like_command_line_option(value)) {\n\tif (bare)\n\t\tflags |= GIT_REPOSITORY_OPEN_BARE;\n\n\terror = git_repository_open_ext(subrepo, path.ptr, flags, wd);\n\n\t/* if we opened the submodule successfully, grab HEAD OID, etc. */\n")
set(_submodule_to
    "\twhile ((error = git_config_next(&entry, iter)) == 0) {\n\t\tconst char *fdot, *ldot;\n\n\t\tif (!entry->value)\n\t\t\tcontinue;\n\n\t\tfdot = strchr(entry->name, '.');\n\t\tldot = strrchr(entry->name, '.');\n\n\t\tif (git_submodule_namemap_contains(names, entry->value)) {\n}\n\tif (entry->value && !strcmp(entry->value, data->path)) {\n\tif ((error = get_value(&value, cfg, &key, sm->name, \"path\")) == 0) {\n\t\tin_config = 1;\n\t\t/* We would warn here if we had that API */\n\t\tif (*value && !looks_like_command_line_option(value)) {\n\tif ((error = get_value(&value, cfg, &key, sm->name, \"url\")) == 0) {\n\t\t/* We would warn here if we had that API */\n\t\tif (!looks_like_command_line_option(value)) {\n\tif (bare)\n\t\tflags |= GIT_REPOSITORY_OPEN_BARE;\n\n\t/* GitBolt: a submodule is opened only strictly inside its\n\t * superproject, and never through a symlink (git's own rule). One that\n\t * is the superproject (`path = /`), or leads out of it (a symlink to\n\t * \".\" or \"..\"), opened a repository status was already reading, and\n\t * status recursed until the stack ran out. */\n\t{\n\t\tgit_str dir = GIT_STR_INIT, real = GIT_STR_INIT, realwd = GIT_STR_INIT;\n\t\tconst char *p = sm->path;\n\t\tint inside = 1, oom = git_str_puts(&dir, wd) < 0;\n\n\t\twhile (!oom && *p && inside) {\n\t\t\tconst char *slash = strchr(p, '/');\n\t\t\tsize_t len = slash ? (size_t)(slash - p) : strlen(p);\n\t\t\tstruct stat st;\n\n\t\t\tif (len) {\n\t\t\t\tif ((dir.size && dir.ptr[dir.size - 1] != '/' &&\n\t\t\t\t     git_str_putc(&dir, '/') < 0) ||\n\t\t\t\t    git_str_put(&dir, p, len) < 0) {\n\t\t\t\t\toom = 1;\n\t\t\t\t\tbreak;\n\t\t\t\t}\n\t\t\t\tif (p_lstat(dir.ptr, &st) == 0 && S_ISLNK(st.st_mode))\n\t\t\t\t\tinside = 0;\n\t\t\t}\n\t\t\tif (!slash)\n\t\t\t\tbreak;\n\t\t\tp = slash + 1;\n\t\t}\n\t\tif (!oom && inside &&\n\t\t    git_fs_path_prettify_dir(&real, dir.ptr, NULL) == 0 &&\n\t\t    git_fs_path_prettify_dir(&realwd, wd, NULL) == 0)\n\t\t\tinside = !git__prefixcmp(real.ptr, realwd.ptr) && real.size > realwd.size;\n\t\tif (git_str_oom(&real) || git_str_oom(&realwd))\n\t\t\toom = 1;\n\t\tif (!oom)\n\t\t\tgit_error_clear();\n\t\tgit_str_dispose(&dir);\n\t\tgit_str_dispose(&real);\n\t\tgit_str_dispose(&realwd);\n\t\tif (oom) {\n\t\t\tgit_str_dispose(&path);\n\t\t\treturn -1;\n\t\t}\n\t\tif (!inside) {\n\t\t\tgit_str_dispose(&path);\n\t\t\tsm->flags |= GIT_SUBMODULE_STATUS__WD_SCANNED;\n\t\t\tgit_error_set(GIT_ERROR_SUBMODULE,\n\t\t\t\t\"submodule '%s' is not inside its superproject\", sm->name);\n\t\t\treturn GIT_EINVALID;\n\t\t}\n\t}\n\n\terror = git_repository_open_ext(subrepo, path.ptr, flags, wd);\n\n\t/* GitBolt: nor may the repository opened have its work tree at the\n\t * superproject's, or above it (core.worktree, or a worktree's gitdir\n\t * file, can put it there): status would read the superproject again. */\n\tif (!error && !bare && git_repository_workdir(*subrepo)) {\n\t\tgit_str real = GIT_STR_INIT, realwd = GIT_STR_INIT;\n\t\tint loops = 0, oom = 0;\n\n\t\tif (git_fs_path_prettify_dir(&real, git_repository_workdir(*subrepo), NULL) == 0 &&\n\t\t    git_fs_path_prettify_dir(&realwd, wd, NULL) == 0)\n\t\t\tloops = !git__prefixcmp(realwd.ptr, real.ptr);\n\t\telse if (git_str_oom(&real) || git_str_oom(&realwd))\n\t\t\toom = 1;\n\t\telse\n\t\t\tgit_error_clear();\n\t\tgit_str_dispose(&real);\n\t\tgit_str_dispose(&realwd);\n\t\tif (oom || loops) {\n\t\t\tgit_repository_free(*subrepo);\n\t\t\t*subrepo = NULL;\n\t\t\tgit_str_dispose(&path);\n\t\t\tif (oom)\n\t\t\t\treturn -1;\n\t\t\tsm->flags |= GIT_SUBMODULE_STATUS__WD_SCANNED;\n\t\t\tgit_error_set(GIT_ERROR_SUBMODULE,\n\t\t\t\t\"submodule '%s' is not inside its superproject\", sm->name);\n\t\t\treturn GIT_EINVALID;\n\t\t}\n\t}\n\n\t/* if we opened the submodule successfully, grab HEAD OID, etc. */\n")
set(_remote_from
    "\t\tif (git__prefixcmp(url, entry->value))\n\t\t\tcontinue;\n\tif (strcmp(entry->value, data->old_remote_name))\n\t\treturn 0;\n\t\tif (strcmp(remote_name, entry->value))\n\t\t\tcontinue;\n")
set(_remote_to
    "\t\tif (!entry->value || git__prefixcmp(url, entry->value))\n\t\t\tcontinue;\n\tif (!entry->value || strcmp(entry->value, data->old_remote_name))\n\t\treturn 0;\n\t\tif (!entry->value || strcmp(remote_name, entry->value))\n\t\t\tcontinue;\n")
set(_repository_old_from
    "\n\tif ((repo->oid_type = git_oid_type_fromstr(entry->value)) == 0) {\n")
set(_repository_old_to
    "\n\tif (!entry->value) {\n\t\tgit_error_set(GIT_ERROR_REPOSITORY,\n\t\t\t\"missing value for 'extensions.objectformat'\");\n\t\terror = GIT_EINVALID;\n\t} else if ((repo->oid_type = git_oid_type_fromstr(entry->value)) == 0) {\n")
set(_repository_new_from
    "\tif ((error = git_config_get_entry(&entry, config, \"init.defaultbranch\")) == 0 &&\n\t\t*entry->value) {\n")
set(_repository_new_to
    "\tif ((error = git_config_get_entry(&entry, config, \"init.defaultbranch\")) == 0 &&\n\t\tentry->value && *entry->value) {\n")
# Files only the later version patches.
set(_checkout_new_from
    "\t\telse if (error)\n\t\t\tgoto cleanup;\n\t\telse if (strcmp(conflict_style->value, \"merge\") == 0)\n")
set(_checkout_new_to
    "\t\telse if (error)\n\t\t\tgoto cleanup;\n\t\telse if (!conflict_style->value) {\n\t\t\tgit_error_set(GIT_ERROR_CHECKOUT,\n\t\t\t\t\"missing value for 'merge.conflictstyle'\");\n\t\t\terror = -1;\n\t\t\tgit_config_entry_free(conflict_style);\n\t\t\tgoto cleanup;\n\t\t}\n\t\telse if (strcmp(conflict_style->value, \"merge\") == 0)\n")
set(_merge_new_from
    "\t\terror = git_config_get_entry(&entry, cfg, \"merge.default\");\n\n\t\tif (error == 0) {\n\t\t\topts->default_driver = git__strdup(entry->value);\n")
set(_merge_new_to
    "\t\terror = git_config_get_entry(&entry, cfg, \"merge.default\");\n\n\t\tif (error == 0 && !entry->value) {\n\t\t\tgit_error_set(GIT_ERROR_MERGE,\n\t\t\t\t\"missing value for 'merge.default'\");\n\t\t\terror = GIT_EINVALID;\n\t\t\tif (!given || !given->metric) {\n\t\t\t\tgit__free(opts->metric);\n\t\t\t\topts->metric = NULL;\n\t\t\t}\n\t\t\tgoto done;\n\t\t} else if (error == 0) {\n\t\t\topts->default_driver = git__strdup(entry->value);\n")
# The limits on hostile content: wildmatch's budget, the tree depth.
set(_wildmatch_new_from
    "static int dowild(const uchar *p, const uchar *text, unsigned int flags)\n{\n\tuchar p_ch;\n\tconst uchar *pattern = p;\n\n\tfor ( ; (p_ch = *p) != '\\0'; text++, p++) {\n\t\tint matched, match_slash, negated;\n\t\tuchar t_ch, prev_ch;\n\t\tif ((t_ch = *text) == '\\0' && p_ch != '*')\n\t\t\t\t\t    dowild(p + 1, text, flags) == WM_MATCH)\n\t\t\t\tif (!match_slash) {\n\t\t\t\t\tif (strchr((char*)text, '/') != NULL)\n\t\t\t\t\t\treturn WM_NOMATCH;\n\t\t\t\t}\n\t\t\t\treturn WM_MATCH;\n\t\t\t\tconst char *slash = strchr((char*)text, '/');\n\t\t\t\tif (!slash)\n\t\t\t\t\treturn WM_NOMATCH;\n\t\t\t\ttext = (const uchar*)slash;\n\t\t\t\t/* the slash is consumed by the top-level for loop */\n\t\t\t\t\t\tif (t_ch == p_ch)\n\t\t\t\t\t\t\tbreak;\n\t\t\t\t\t\ttext++;\n\t\t\t\t\t}\n\t\t\t\t\tif (t_ch != p_ch)\n\t\t\t\t\t\treturn WM_NOMATCH;\n\t\t\t\t}\n\t\t\t\tif ((matched = dowild(p, text, flags)) != WM_NOMATCH) {\n\t\t\tdo {\n\t\t\t\tif (!p_ch)\n\t\t\t\t\treturn WM_ABORT_ALL;\n\t\t\t\tif (p_ch == '\\\\') {\n\t\t\t\t\tfor (s = p += 2; (p_ch = *p) && p_ch != ']'; p++) {} /*SHARED ITERATOR*/\n\t\t\t\t\tif (!p_ch)\n\t\t\t\t\t\treturn WM_ABORT_ALL;\n\t\t\t\t\ti = (int)(p - s - 1);\nint wildmatch(const char *pattern, const char *text, unsigned int flags)\n{\n\treturn dowild((const uchar*)pattern, (const uchar*)text, flags);\n}\n")
set(_wildmatch_new_to
    "static int dowild(const uchar *p, const uchar *text, unsigned int flags, long *budget)\n{\n\tuchar p_ch;\n\tconst uchar *pattern = p;\n\n\tfor ( ; (p_ch = *p) != '\\0'; text++, p++) {\n\t\tint matched, match_slash, negated;\n\t\tuchar t_ch, prev_ch;\n\t\t/* GitBolt: a hostile pattern backtracks exponentially, so a\n\t\t * match has a budget of steps (see wildmatch()), spent here and\n\t\t * in every loop that skips over the path or the pattern. Spent,\n\t\t * the pattern doesn't match. */\n\t\tif (--*budget <= 0)\n\t\t\treturn WM_ABORT_ALL;\n\t\tif ((t_ch = *text) == '\\0' && p_ch != '*')\n\t\t\t\t\t    dowild(p + 1, text, flags, budget) == WM_MATCH)\n\t\t\t\tif (!match_slash) {\n\t\t\t\t\tconst char *slash = strchr((char*)text, '/');\n\t\t\t\t\tif (slash != NULL) {\n\t\t\t\t\t\t*budget -= (long)(slash - (const char*)text);\n\t\t\t\t\t\treturn WM_NOMATCH;\n\t\t\t\t\t}\n\t\t\t\t}\n\t\t\t\treturn WM_MATCH;\n\t\t\t\tconst char *slash = strchr((char*)text, '/');\n\t\t\t\tif (!slash) {\n\t\t\t\t\t*budget -= (long)strlen((char*)text);\n\t\t\t\t\treturn WM_NOMATCH;\n\t\t\t\t}\n\t\t\t\t*budget -= (long)(slash - (const char*)text);\n\t\t\t\ttext = (const uchar*)slash;\n\t\t\t\t/* the slash is consumed by the top-level for loop */\n\t\t\t\t\t\tif (t_ch == p_ch)\n\t\t\t\t\t\t\tbreak;\n\t\t\t\t\t\tif (--*budget <= 0)\n\t\t\t\t\t\t\treturn WM_ABORT_ALL;\n\t\t\t\t\t\ttext++;\n\t\t\t\t\t}\n\t\t\t\t\tif (t_ch != p_ch)\n\t\t\t\t\t\treturn WM_NOMATCH;\n\t\t\t\t}\n\t\t\t\tif ((matched = dowild(p, text, flags, budget)) != WM_NOMATCH) {\n\t\t\tdo {\n\t\t\t\tif (!p_ch || --*budget <= 0)\n\t\t\t\t\treturn WM_ABORT_ALL;\n\t\t\t\tif (p_ch == '\\\\') {\n\t\t\t\t\tfor (s = p += 2; (p_ch = *p) && p_ch != ']'; p++) {} /*SHARED ITERATOR*/\n\t\t\t\t\tif (!p_ch || (*budget -= (long)(p - s)) <= 0)\n\t\t\t\t\t\treturn WM_ABORT_ALL;\n\t\t\t\t\ti = (int)(p - s - 1);\nint wildmatch(const char *pattern, const char *text, unsigned int flags)\n{\n\t/* GitBolt: an ordinary pattern takes one to three steps per\n\t * character of the path. A match gets 1000, and if they run out,\n\t * starts over with 64 per character of the pattern and the path. */\n\tlong budget = 1000;\n\tsize_t len;\n\tint error = dowild((const uchar*)pattern, (const uchar*)text, flags, &budget);\n\n\tif (error != WM_ABORT_ALL || budget > 0)\n\t\treturn error;\n\tlen = strlen(pattern) + strlen(text);\n\tbudget = 1000 + 64 * (long)(len < 1000000 ? len : 1000000);\n\treturn dowild((const uchar*)pattern, (const uchar*)text, flags, &budget);\n}\n")
set(_tree_h_new_from
    "GIT_INLINE(bool) git_tree_entry__is_tree(const struct git_tree_entry *e)\n{\n\treturn (S_ISDIR(e->attr) && !S_ISGITLINK(e->attr));\n}\n\nvoid git_tree__free(void *tree);\n")
set(_tree_h_new_to
    "GIT_INLINE(bool) git_tree_entry__is_tree(const struct git_tree_entry *e)\n{\n\treturn (S_ISDIR(e->attr) && !S_ISGITLINK(e->attr));\n}\n\n/*\n * GitBolt: how deep the tree iterator and git_tree_walk() go. A tree\n * nested thousands of levels deep costs a few kilobytes of objects; the\n * iterator, which keeps the path of every directory it is in, then held\n * the square of the depth (gigabytes), and git_tree_walk() recursed\n * until the stack ran out. git stops at 2048 levels too (its\n * core.maxTreeDepth). A directory's path is held to GIT_PATH_MAX, or\n * long names would make even that depth dear.\n */\n#define GIT_TREE_MAX_DEPTH 2048\n\n/*\n * Whether a walk among the entries of a tree `depth` levels down (1 for\n * the root tree's) may go into one of them, a tree whose path (with its\n * trailing slash) is `path_len` bytes long.\n */\nGIT_INLINE(int) git_tree__descend_ok(size_t depth, size_t path_len)\n{\n\tif (depth >= GIT_TREE_MAX_DEPTH) {\n\t\tgit_error_set(GIT_ERROR_TREE,\n\t\t\t\"tree too deep: more than %d levels\", GIT_TREE_MAX_DEPTH);\n\t\treturn GIT_EINVALID;\n\t}\n\tif (path_len > GIT_PATH_MAX) {\n\t\tgit_error_set(GIT_ERROR_TREE,\n\t\t\t\"tree too deep: a path of more than %d bytes\", GIT_PATH_MAX);\n\t\treturn GIT_EINVALID;\n\t}\n\treturn 0;\n}\n\nvoid git_tree__free(void *tree);\n")
set(_iterator_new_from
    "\tgit_tree *tree = NULL;\n\tint error;\n\n\tif ((error = git_tree_lookup(&tree,\n\t\t\titer->base.repo, &entry->tree_entry->oid)) < 0 ||\n\t\t(error = tree_iterator_frame_init(iter, tree, entry)) < 0)\n")
set(_iterator_new_to
    "\tgit_tree *tree = NULL;\n\tint error;\n\n\t/* GitBolt: no deeper than GIT_TREE_MAX_DEPTH (see tree.h). */\n\tif ((error = git_tree__descend_ok(iter->frames.size,\n\t\t\t(entry->parent_path ? strlen(entry->parent_path) : 0) +\n\t\t\tentry->tree_entry->filename_len + 1)) < 0)\n\t\treturn error;\n\n\tif ((error = git_tree_lookup(&tree,\n\t\t\titer->base.repo, &entry->tree_entry->oid)) < 0 ||\n\t\t(error = tree_iterator_frame_init(iter, tree, entry)) < 0)\n")
set(_tree_c_new_from
    "\tgit_str *path,\n\tvoid *payload,\n\tbool preorder)\n{\n\tint error = 0;\n\tsize_t i;\n\tconst git_tree_entry *entry;\n\n\t\t\tsize_t path_len = git_str_len(path);\n\n\t\t\terror = git_tree_lookup(&subtree, tree->object.repo, &entry->oid);\n\t\t\tif (error < 0)\n\t\t\t\tbreak;\n\n\t\t\t/* append the next entry to the path */\n\t\t\tgit_str_puts(path, entry->filename);\n\t\t\tgit_str_putc(path, '/');\n\n\t\t\tif (git_str_oom(path))\n\t\t\t\terror = -1;\n\t\t\telse\n\t\t\t\terror = tree_walk(subtree, callback, path, payload, preorder);\n\n\terror = tree_walk(\n\t\ttree, callback, &root_path, payload, (mode == GIT_TREEWALK_PRE));\n")
set(_tree_c_new_to
    "\tgit_str *path,\n\tvoid *payload,\n\tbool preorder,\n\tsize_t depth)\n{\n\tint error = 0;\n\tsize_t i;\n\tconst git_tree_entry *entry;\n\n\t\t\tsize_t path_len = git_str_len(path);\n\n\t\t\t/* GitBolt: no deeper than GIT_TREE_MAX_DEPTH (see tree.h). */\n\t\t\terror = git_tree__descend_ok(depth,\n\t\t\t\tpath_len + entry->filename_len + 1);\n\t\t\tif (error < 0)\n\t\t\t\tbreak;\n\n\t\t\terror = git_tree_lookup(&subtree, tree->object.repo, &entry->oid);\n\t\t\tif (error < 0)\n\t\t\t\tbreak;\n\n\t\t\t/* append the next entry to the path */\n\t\t\tgit_str_puts(path, entry->filename);\n\t\t\tgit_str_putc(path, '/');\n\n\t\t\tif (git_str_oom(path))\n\t\t\t\terror = -1;\n\t\t\telse\n\t\t\t\terror = tree_walk(subtree, callback, path, payload, preorder, depth + 1);\n\n\terror = tree_walk(\n\t\ttree, callback, &root_path, payload, (mode == GIT_TREEWALK_PRE), 1);\n")
foreach(_file config config_file repository)
    set(_${_file}_from "${_${_file}_old_from}${_${_file}_new_from}")
    set(_${_file}_to "${_${_file}_old_to}${_${_file}_new_to}")
    # As the 0.9.7 script left it.
    set(_${_file}_097 "${_${_file}_old_to}${_${_file}_new_from}")
endforeach()
foreach(_file checkout merge wildmatch tree_h iterator tree_c)
    set(_${_file}_from "${_${_file}_new_from}")
    set(_${_file}_to "${_${_file}_new_to}")
    set(_${_file}_097 "${_${_file}_new_from}")
endforeach()
foreach(_file cflags common submodule remote)
    set(_${_file}_097 "${_${_file}_to}")
endforeach()

# A tree with the thirteen files: <suffix> picks each one's text (_from,
# _to or _097), <config_file> config_file.c's.
function(write_tree dir suffix config_file)
    file(REMOVE_RECURSE ${dir})
    file(WRITE ${dir}/cmake/DefaultCFlags.cmake "${_cflags${suffix}}")
    file(WRITE ${dir}/include/git2/common.h "${_common${suffix}}")
    file(WRITE ${dir}/src/libgit2/config.c "${_config${suffix}}")
    file(WRITE ${dir}/src/libgit2/config_file.c "${config_file}")
    file(WRITE ${dir}/src/libgit2/submodule.c "${_submodule${suffix}}")
    file(WRITE ${dir}/src/libgit2/remote.c "${_remote${suffix}}")
    file(WRITE ${dir}/src/libgit2/repository.c "${_repository${suffix}}")
    file(WRITE ${dir}/src/libgit2/checkout.c "${_checkout${suffix}}")
    file(WRITE ${dir}/src/libgit2/merge.c "${_merge${suffix}}")
    file(WRITE ${dir}/src/util/wildmatch.c "${_wildmatch${suffix}}")
    file(WRITE ${dir}/src/libgit2/tree.h "${_tree_h${suffix}}")
    file(WRITE ${dir}/src/libgit2/iterator.c "${_iterator${suffix}}")
    file(WRITE ${dir}/src/libgit2/tree.c "${_tree_c${suffix}}")
endfunction()

function(run_patch dir result)
    execute_process(COMMAND ${CMAKE_COMMAND} -DPATCH_SHA256=test -P ${PATCH_SCRIPT}
        WORKING_DIRECTORY ${dir}
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _out)
    set(${result} ${_rc} PARENT_SCOPE)
    set(${result}_OUTPUT "${_out}" PARENT_SCOPE)
endfunction()

function(expect_patched what dir)
    foreach(_pair "cmake/DefaultCFlags.cmake;_cflags_to" "include/git2/common.h;_common_to"
                  "src/libgit2/config.c;_config_to" "src/libgit2/config_file.c;_config_file_to"
                  "src/libgit2/submodule.c;_submodule_to" "src/libgit2/remote.c;_remote_to"
                  "src/libgit2/repository.c;_repository_to" "src/libgit2/checkout.c;_checkout_to"
                  "src/libgit2/merge.c;_merge_to" "src/util/wildmatch.c;_wildmatch_to"
                  "src/libgit2/tree.h;_tree_h_to" "src/libgit2/iterator.c;_iterator_to"
                  "src/libgit2/tree.c;_tree_c_to")
        list(GET _pair 0 _file)
        list(GET _pair 1 _expected)
        file(READ ${dir}/${_file} _text)
        if(NOT _text STREQUAL "${${_expected}}")
            message(FATAL_ERROR "${what}: ${_file} is\n${_text}\nnot\n${${_expected}}")
        endif()
    endforeach()
endfunction()

set(_tree ${WORK_DIR}/tree)

# A fresh tree: patched.
write_tree(${_tree} _from "${_config_file_from}")
run_patch(${_tree} _rc)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "fresh tree: the script failed:\n${_rc_OUTPUT}")
endif()
expect_patched("fresh tree" ${_tree})

# Again over the same tree: nothing changes.
run_patch(${_tree} _rc)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "patched tree: the script failed:\n${_rc_OUTPUT}")
endif()
expect_patched("patched tree" ${_tree})

# A tree the 0.9.7 script patched, as FetchContent runs a newer one over
# it: it gets the newer patches.
write_tree(${_tree} _097 "${_config_file_097}")
run_patch(${_tree} _rc)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "a tree 0.9.7 patched: the script failed:\n${_rc_OUTPUT}")
endif()
expect_patched("a tree 0.9.7 patched" ${_tree})

# A tree patched in a way the script doesn't know (an earlier version of
# it, say): fails, rather than leaving it as it is.
string(REPLACE "\n\n\treturn" "\n\tgitbolt_unknown();\n\n\treturn" _unknown "${_config_file_from}")
write_tree(${_tree} _to "${_unknown}")
run_patch(${_tree} _rc)
if(_rc EQUAL 0)
    message(FATAL_ERROR "a config_file.c patched in an unknown way: the script succeeded")
endif()
if(NOT _rc_OUTPUT MATCHES "config_file\\.c")
    message(FATAL_ERROR "a config_file.c patched in an unknown way: the error doesn't name the file:\n${_rc_OUTPUT}")
endif()

message(STATUS "PatchLibgit2.cmake: all cases passed")
