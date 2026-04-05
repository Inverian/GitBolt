add_executable(gitbolt_tests
    git/TestRepository.cpp
)

target_link_libraries(gitbolt_tests PRIVATE
    gitbolt_git
    Qt6::Test
)

add_test(NAME gitbolt_tests COMMAND gitbolt_tests)
