file(GLOB SOURCES "*.cpp" "*.h")
add_library(gitbolt_git STATIC ${SOURCES})

target_include_directories(gitbolt_git PUBLIC ${CMAKE_SOURCE_DIR}/src)

# Link libgit2
if(TARGET PkgConfig::LIBGIT2)
    target_link_libraries(gitbolt_git PUBLIC PkgConfig::LIBGIT2)
elseif(TARGET libgit2::git2)
    target_link_libraries(gitbolt_git PUBLIC libgit2::git2)
elseif(TARGET git2)
    target_link_libraries(gitbolt_git PUBLIC git2)
endif()

target_link_libraries(gitbolt_git PUBLIC gitbolt_util)
target_link_libraries(gitbolt_git PRIVATE Qt6::Core)
gitbolt_set_warnings(gitbolt_git)
