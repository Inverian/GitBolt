function(gitbolt_set_warnings target)
    target_compile_options(${target} PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:
            -Wall -Wextra -Wpedantic
            -Wno-unused-parameter
            -Wshadow
            -Wformat=2
            -Wconversion
            -Wnon-virtual-dtor
        >
        $<$<CXX_COMPILER_ID:MSVC>:
            /W4 /permissive-
        >
    )
endfunction()
