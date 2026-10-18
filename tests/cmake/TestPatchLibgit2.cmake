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
set(_config_from
    "\t\tif (res != GIT_ENOTFOUND) {\n\t\t\t*out = &be->entry;\n\t\t\tbreak;\n\t\t}\n")
set(_config_to
    "\t\tif (res != GIT_ENOTFOUND) {\n\t\t\tif (res == 0)\n\t\t\t\t*out = &be->entry;\n\t\t\tbreak;\n\t\t}\n")
set(_config_file_from
    "out:\n\tgit_config_list_free(config_list);\n\n\treturn (error == GIT_ENOTFOUND) ? 0 : error;\n}\n")
set(_config_file_to
    "out:\n\tgit_config_list_free(config_list);\n\tif (error < 0) {\n\t\tgit_futils_filestamp_set(&b->file.stamp, NULL);\n\t\tmemset(b->file.checksum, 0, sizeof(b->file.checksum));\n\t}\n\n\treturn (error == GIT_ENOTFOUND) ? 0 : error;\n}\n")
# The config-lines-with-no-value fixes.
set(_submodule_from
    "\twhile ((error = git_config_next(&entry, iter)) == 0) {\n\t\tconst char *fdot, *ldot;\n\n\t\tfdot = strchr(entry->name, '.');\n\t\tldot = strrchr(entry->name, '.');\n\n\t\tif (git_submodule_namemap_contains(names, entry->value)) {\n}\n\tif (!strcmp(entry->value, data->path)) {\n\tif ((error = get_value(&value, cfg, &key, sm->name, \"path\")) == 0) {\n\t\tin_config = 1;\n\t\t/* We would warn here if we had that API */\n\t\tif (!looks_like_command_line_option(value)) {\n\tif ((error = get_value(&value, cfg, &key, sm->name, \"url\")) == 0) {\n\t\t/* We would warn here if we had that API */\n\t\tif (!looks_like_command_line_option(value)) {\n\tif (bare)\n\t\tflags |= GIT_REPOSITORY_OPEN_BARE;\n\n\terror = git_repository_open_ext(subrepo, path.ptr, flags, wd);\n\n\t/* if we opened the submodule successfully, grab HEAD OID, etc. */\n")
set(_submodule_to
    "\twhile ((error = git_config_next(&entry, iter)) == 0) {\n\t\tconst char *fdot, *ldot;\n\n\t\tif (!entry->value)\n\t\t\tcontinue;\n\n\t\tfdot = strchr(entry->name, '.');\n\t\tldot = strrchr(entry->name, '.');\n\n\t\tif (git_submodule_namemap_contains(names, entry->value)) {\n}\n\tif (entry->value && !strcmp(entry->value, data->path)) {\n\tif ((error = get_value(&value, cfg, &key, sm->name, \"path\")) == 0) {\n\t\tin_config = 1;\n\t\t/* We would warn here if we had that API */\n\t\tif (*value && !looks_like_command_line_option(value)) {\n\tif ((error = get_value(&value, cfg, &key, sm->name, \"url\")) == 0) {\n\t\t/* We would warn here if we had that API */\n\t\tif (!looks_like_command_line_option(value)) {\n\tif (bare)\n\t\tflags |= GIT_REPOSITORY_OPEN_BARE;\n\n\t/* GitBolt: the submodule's folder must be strictly inside its\n\t * superproject's: one that is the superproject, or leads out of it\n\t * (through a symlink to \".\" or \"..\"), loops back to a repository\n\t * being scanned, and status recursed until the stack ran out. */\n\t{\n\t\tgit_str dir = GIT_STR_INIT, real = GIT_STR_INIT, realwd = GIT_STR_INIT;\n\t\tint inside = 1;\n\n\t\tif (git_str_join(&dir, '/', wd, sm->path) < 0) {\n\t\t\tgit_str_dispose(&path);\n\t\t\treturn -1;\n\t\t}\n\t\tif (git_fs_path_prettify_dir(&real, dir.ptr, NULL) == 0 &&\n\t\t    git_fs_path_prettify_dir(&realwd, wd, NULL) == 0)\n\t\t\tinside = !git__prefixcmp(real.ptr, realwd.ptr) && real.size > realwd.size;\n\t\telse\n\t\t\tgit_error_clear();\n\t\tgit_str_dispose(&dir);\n\t\tgit_str_dispose(&real);\n\t\tgit_str_dispose(&realwd);\n\t\tif (!inside) {\n\t\t\tgit_str_dispose(&path);\n\t\t\tsm->flags |= GIT_SUBMODULE_STATUS__WD_SCANNED;\n\t\t\tgit_error_set(GIT_ERROR_SUBMODULE,\n\t\t\t\t\"submodule '%s' is not inside its superproject\", sm->name);\n\t\t\treturn GIT_EINVALID;\n\t\t}\n\t}\n\n\terror = git_repository_open_ext(subrepo, path.ptr, flags, wd);\n\n\t/* if we opened the submodule successfully, grab HEAD OID, etc. */\n")
set(_remote_from
    "\t\tif (git__prefixcmp(url, entry->value))\n\t\t\tcontinue;\n\tif (strcmp(entry->value, data->old_remote_name))\n\t\treturn 0;\n\t\tif (strcmp(remote_name, entry->value))\n\t\t\tcontinue;\n")
set(_remote_to
    "\t\tif (!entry->value || git__prefixcmp(url, entry->value))\n\t\t\tcontinue;\n\tif (!entry->value || strcmp(entry->value, data->old_remote_name))\n\t\treturn 0;\n\t\tif (!entry->value || strcmp(remote_name, entry->value))\n\t\t\tcontinue;\n")
set(_repository_from
    "\n\tif ((repo->oid_type = git_oid_type_fromstr(entry->value)) == 0) {\n")
set(_repository_to
    "\n\tif (!entry->value) {\n\t\tgit_error_set(GIT_ERROR_REPOSITORY,\n\t\t\t\"missing value for 'extensions.objectformat'\");\n\t\terror = GIT_EINVALID;\n\t} else if ((repo->oid_type = git_oid_type_fromstr(entry->value)) == 0) {\n")

# A tree with the seven files: <suffix> picks each one's text (_from or
# _to), <config_file> config_file.c's.
function(write_tree dir suffix config_file)
    file(REMOVE_RECURSE ${dir})
    file(WRITE ${dir}/cmake/DefaultCFlags.cmake "${_cflags${suffix}}")
    file(WRITE ${dir}/include/git2/common.h "${_common${suffix}}")
    file(WRITE ${dir}/src/libgit2/config.c "${_config${suffix}}")
    file(WRITE ${dir}/src/libgit2/config_file.c "${config_file}")
    file(WRITE ${dir}/src/libgit2/submodule.c "${_submodule${suffix}}")
    file(WRITE ${dir}/src/libgit2/remote.c "${_remote${suffix}}")
    file(WRITE ${dir}/src/libgit2/repository.c "${_repository${suffix}}")
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
                  "src/libgit2/repository.c;_repository_to")
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
