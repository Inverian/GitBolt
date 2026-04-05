add_executable(gitbolt_tests
    core/test_repository.cpp
)

target_link_libraries(gitbolt_tests PRIVATE
    gitbolt_core
    Qt6::Test
)

add_test(NAME gitbolt_tests COMMAND gitbolt_tests)
