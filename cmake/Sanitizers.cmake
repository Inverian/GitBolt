# Included from the top-level CMakeLists when GITBOLT_SANITIZERS=ON.
# Global on purpose: the old per-target gitbolt_enable_sanitizers()
# function was never called on any target, so the option silently
# built uninstrumented binaries while the docs claimed ASan+UBSan.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
    message(STATUS "GitBolt: building with ASan + UBSan")
else()
    message(WARNING "GITBOLT_SANITIZERS is only wired for GCC/Clang")
endif()
