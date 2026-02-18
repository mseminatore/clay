cmake_minimum_required(VERSION 3.10)
project(clay C)

set(CMAKE_C_STANDARD 99)

# Suppress MSVC deprecation warnings
if(MSVC)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

# Build hash library directly (avoid its CMakeLists which has subdirectory conflicts)
add_library(ht STATIC deps/hash/hash.c)
target_include_directories(ht PUBLIC deps/hash)

# Build testy library directly
add_library(testy STATIC deps/testy/test_main.c)
target_include_directories(testy PUBLIC deps/testy)

# Runtime library
set(RUNTIME_SOURCES
    src/runtime/object.c
    src/runtime/memory.c
    src/runtime/dispatch.c
    src/runtime/tagged.c
    src/runtime/string.c
    src/runtime/number.c
    src/runtime/block.c
    src/runtime/array.c
    src/runtime/bootstrap.c
)

add_library(clay_runtime STATIC ${RUNTIME_SOURCES})
target_include_directories(clay_runtime PUBLIC src/runtime)
target_link_libraries(clay_runtime PUBLIC ht)

# Compiler binary
add_executable(clay_compiler
    src/compiler/main.c
    src/compiler/lexer.c
    src/compiler/parser.c
    src/compiler/ast.c
    src/compiler/codegen.c
)
target_include_directories(clay_compiler PRIVATE src/compiler)
target_compile_definitions(clay_compiler PRIVATE _CRT_SECURE_NO_WARNINGS)
set_target_properties(clay_compiler PROPERTIES OUTPUT_NAME clay)

# Test runner
add_executable(test_runner test/test_runtime.c)
target_link_libraries(test_runner clay_runtime testy)

enable_testing()
add_test(NAME clay_tests COMMAND test_runner)
