cmake_minimum_required(VERSION 3.10)
project(clay C)

# compile_commands.json for clangd / editor tooling
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_C_STANDARD 99)

# Thread-safety option (off by default — zero overhead when disabled)
option(CLAY_THREAD_SAFE "Enable thread-safe refcounting and TLS for multi-threaded use" OFF)

# Suppress MSVC deprecation warnings
if(MSVC)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

if(CLAY_THREAD_SAFE)
    add_compile_definitions(CLAY_THREAD_SAFE)
    if(NOT WIN32)
        find_package(Threads REQUIRED)
    endif()
endif()

# Suppress typedef redefinition warning (generated VM code uses C11 pattern)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|AppleClang")
    add_compile_options(-Wno-typedef-redefinition)
endif()

# Crossline for cross-platform REPL line editing
add_library(crossline STATIC deps/crossline/crossline.c)
target_include_directories(crossline PUBLIC deps/crossline)
if(MSVC)
    target_compile_definitions(crossline PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# cJSON for DAP debug adapter protocol
add_library(cjson STATIC deps/cjson/cJSON.c)
target_include_directories(cjson PUBLIC deps/cjson)
if(MSVC)
    target_compile_definitions(cjson PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# Link math library on Unix (via clay_runtime, which needs it)
if(UNIX)
    # Optional OpenSSL for HTTPS support
    find_package(OpenSSL QUIET)
    if(OPENSSL_FOUND)
        message(STATUS "OpenSSL found — HTTPS enabled on Unix")
    else()
        message(STATUS "OpenSSL not found — HTTPS disabled on Unix (HTTP only)")
    endif()
endif()

# Build hash library directly (avoid its CMakeLists which has subdirectory conflicts)
add_library(ht STATIC deps/hash/hash.c)
target_include_directories(ht PUBLIC deps/hash)
# The runtime frees slot keys/values itself and then frees non-empty tables on
# purpose, so hash's debug-build "freed with N entries" warning (on by default
# when _DEBUG is defined, i.e. MSVC Debug) is a false positive here.
target_compile_definitions(ht PRIVATE HT_WARN_FREE_NONEMPTY=0)
if(UNIX)
    target_compile_definitions(ht PRIVATE _POSIX_C_SOURCE=200809L)
endif()
if(CLAY_THREAD_SAFE)
    target_compile_definitions(ht PRIVATE HT_THREAD_SAFE)
endif()

# Build testy library directly
add_library(testy STATIC deps/testy/test_main.c)
target_include_directories(testy PUBLIC deps/testy)

# Runtime library
set(RUNTIME_SOURCES
    src/runtime/object.c
    src/runtime/vm_bridge.c
    src/runtime/memory.c
    src/runtime/dispatch.c
    src/runtime/tagged.c
    src/runtime/string.c
    src/runtime/number.c
    src/runtime/block.c
    src/runtime/array.c
    src/runtime/error.c
    src/runtime/clay_dict.c
    src/runtime/range.c
    src/runtime/math.c
    src/runtime/file.c
    src/runtime/ffi.c
    src/runtime/date.c
    src/runtime/http.c
    src/runtime/regex.c
    src/runtime/crypto.c
    src/runtime/process.c
    src/runtime/db.c
    src/runtime/bootstrap.c
    src/runtime/intern.c
    src/runtime/graphics.c
    src/runtime/vec3.c
    src/runtime/thread.c
    src/runtime/plugin.c
    src/runtime/fiber.c
)

# SQLite amalgamation
add_library(sqlite3 STATIC deps/sqlite/sqlite3.c)
target_include_directories(sqlite3 PUBLIC deps/sqlite)
if(MSVC)
    target_compile_definitions(sqlite3 PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# Optional SDL2 for Graphics prototype
find_package(SDL2 CONFIG QUIET)
if(SDL2_FOUND)
    message(STATUS "SDL2 found — Graphics prototype enabled")
else()
    message(STATUS "SDL2 not found — Graphics prototype disabled (install via vcpkg)")
endif()

# Fiber context switching (assembly on Unix) is part of the runtime so natively
# compiled programs (`clay --compile`) get it too
if(NOT WIN32)
    enable_language(ASM)
    list(APPEND RUNTIME_SOURCES
        src/runtime/fiber_ctx.c
        src/runtime/fiber_amd64.S
        src/runtime/fiber_arm64.S
    )
endif()

add_library(clay_runtime STATIC ${RUNTIME_SOURCES})
target_include_directories(clay_runtime PUBLIC src/runtime deps/sqlite src/vm/generated)
# The VM bridge (vm_bridge.c) runs bytecode in the VM; Console readLine edits
# lines with crossline
target_link_libraries(clay_runtime PUBLIC ht sqlite3 clayvm crossline)

if(SDL2_FOUND)
    target_compile_definitions(clay_runtime PRIVATE CLAY_HAS_SDL2)
    target_link_libraries(clay_runtime PUBLIC
        $<TARGET_NAME_IF_EXISTS:SDL2::SDL2main>
        $<IF:$<TARGET_EXISTS:SDL2::SDL2>,SDL2::SDL2,SDL2::SDL2-static>
    )
endif()
if(WIN32)
    target_link_libraries(clay_runtime PUBLIC winhttp ws2_32 bcrypt)
endif()
if(UNIX)
    target_compile_definitions(clay_runtime PRIVATE _POSIX_C_SOURCE=200809L)
    target_link_libraries(clay_runtime PUBLIC m dl)
    if(CLAY_THREAD_SAFE)
        target_link_libraries(clay_runtime PUBLIC Threads::Threads)
    endif()
    if(OPENSSL_FOUND)
        target_compile_definitions(clay_runtime PRIVATE CLAY_HAS_SSL)
        target_include_directories(clay_runtime PRIVATE ${OPENSSL_INCLUDE_DIR})
        target_link_libraries(clay_runtime PUBLIC OpenSSL::SSL OpenSSL::Crypto)
    endif()
endif()

# VM library (generated by vmgen from src/vm/clay.yaml — see cmake/regen_vm.cmake)
# Regenerate after editing clay.yaml:  cmake --build build --target regen_vm
add_custom_target(regen_vm
    COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/regen_vm.cmake
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Regenerating src/vm/generated from src/vm/clay.yaml"
    VERBATIM)

set(VM_SOURCES
    src/vm/generated/vm.c
    src/vm/generated/native.c
    src/vm/generated/assembler.c
    src/vm/generated/disasm.c
    src/vm/generated/strings.c
    src/vm/generated/bytecode.c
    src/vm/generated/verify.c
    src/vm/generated/optimizer.c
)

add_library(clayvm STATIC ${VM_SOURCES})
target_include_directories(clayvm PUBLIC src/vm/generated)
if(UNIX)
    target_compile_definitions(clayvm PRIVATE _POSIX_C_SOURCE=200809L)
endif()

# Compiler library (for testing — everything except main.c)
set(COMPILER_LIB_SOURCES
    src/compiler/lexer.c
    src/compiler/parser.c
    src/compiler/ast.c
    src/compiler/codegen.c
    src/compiler/bc_compiler.c
    src/compiler/bricks.c
    src/compiler/check.c
    src/compiler/native_bricks.c
    src/compiler/dap.c
)

add_library(clay_compiler_lib STATIC ${COMPILER_LIB_SOURCES})
target_include_directories(clay_compiler_lib PUBLIC src/compiler src/vm/generated src/runtime deps/cjson)
target_compile_definitions(clay_compiler_lib PRIVATE _CRT_SECURE_NO_WARNINGS)
if(UNIX)
    target_compile_definitions(clay_compiler_lib PRIVATE _POSIX_C_SOURCE=200809L)
endif()
target_link_libraries(clay_compiler_lib clayvm clay_runtime cjson)

# Compiler binary
add_executable(clay_compiler
    src/compiler/main.c
)
target_include_directories(clay_compiler PRIVATE src/compiler src/vm/generated src/runtime deps/crossline deps/cjson)
target_compile_definitions(clay_compiler PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(clay_compiler clay_compiler_lib)
if(UNIX)
    target_compile_definitions(clay_compiler PRIVATE _POSIX_C_SOURCE=200809L)
endif()
set_target_properties(clay_compiler PROPERTIES OUTPUT_NAME clay)

# --- Native compile configuration (clay --compile) ---
# Written next to the clay binary: how to compile generated C and link it
# against the prebuilt runtime, with the same options/features as this build.
set(CLAY_NATIVE_DEFS "")
if(CLAY_THREAD_SAFE)
    set(CLAY_NATIVE_DEFS "-DCLAY_THREAD_SAFE")
endif()
set(CLAY_NATIVE_LIBS
    "\"$<TARGET_FILE:clay_runtime>\" \"$<TARGET_FILE:clayvm>\" \"$<TARGET_FILE:crossline>\" \"$<TARGET_FILE:ht>\" \"$<TARGET_FILE:sqlite3>\"")
if(SDL2_FOUND)
    string(APPEND CLAY_NATIVE_LIBS
        " \"$<TARGET_LINKER_FILE:$<IF:$<TARGET_EXISTS:SDL2::SDL2>,SDL2::SDL2,SDL2::SDL2-static>>\"")
endif()
if(MSVC)
    set(CLAY_NATIVE_KIND "msvc")
    # Match the C runtime the libraries were built with (CMake's default:
    # dynamic, debug in Debug) — mixing /MT and /MDd fails to link
    if(CMAKE_MSVC_RUNTIME_LIBRARY MATCHES "DLL$" OR NOT CMAKE_MSVC_RUNTIME_LIBRARY)
        set(CLAY_NATIVE_CRT "$<IF:$<CONFIG:Debug>,/MDd,/MD>")
    else()
        set(CLAY_NATIVE_CRT "$<IF:$<CONFIG:Debug>,/MTd,/MT>")
    endif()
    set(CLAY_NATIVE_CFLAGS "/nologo /O2 ${CLAY_NATIVE_CRT} ${CMAKE_C_FLAGS} /D_CRT_SECURE_NO_WARNINGS ${CLAY_NATIVE_DEFS} /I\"${CMAKE_CURRENT_SOURCE_DIR}/src/runtime\" /I\"${CMAKE_CURRENT_SOURCE_DIR}/deps/hash\"")
    # user32: crossline (Console readLine) reads key states
    string(APPEND CLAY_NATIVE_LIBS " winhttp.lib ws2_32.lib user32.lib bcrypt.lib")
else()
    set(CLAY_NATIVE_KIND "cc")
    set(CLAY_NATIVE_CFLAGS "-std=c99 -O2 ${CMAKE_C_FLAGS} ${CLAY_NATIVE_DEFS} -I\"${CMAKE_CURRENT_SOURCE_DIR}/src/runtime\" -I\"${CMAKE_CURRENT_SOURCE_DIR}/deps/hash\"")
    if(APPLE)
        # The raw toolchain compiler needs the SDK passed explicitly
        set(CLAY_NATIVE_SYSROOT "${CMAKE_OSX_SYSROOT}")
        if(NOT CLAY_NATIVE_SYSROOT)
            execute_process(COMMAND xcrun --show-sdk-path
                OUTPUT_VARIABLE CLAY_NATIVE_SYSROOT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        endif()
        if(CLAY_NATIVE_SYSROOT)
            string(APPEND CLAY_NATIVE_CFLAGS " -isysroot \"${CLAY_NATIVE_SYSROOT}\"")
        endif()
    endif()
    if(OPENSSL_FOUND)
        string(APPEND CLAY_NATIVE_LIBS " \"$<TARGET_LINKER_FILE:OpenSSL::SSL>\" \"$<TARGET_LINKER_FILE:OpenSSL::Crypto>\"")
    endif()
    string(APPEND CLAY_NATIVE_LIBS " -lm -ldl")
    if(CLAY_THREAD_SAFE OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
        string(APPEND CLAY_NATIVE_LIBS " -lpthread")
    endif()
endif()
file(GENERATE OUTPUT "$<TARGET_FILE_DIR:clay_compiler>/clay-native.cfg" CONTENT
"# Generated by CMake — used by `clay --compile` to build native binaries
kind=${CLAY_NATIVE_KIND}
cc=${CMAKE_C_COMPILER}
cflags=${CLAY_NATIVE_CFLAGS}
libs=${CLAY_NATIVE_LIBS}
")

# Copy SDL2 DLL to output directory (Windows only)
if(WIN32 AND SDL2_FOUND AND TARGET SDL2::SDL2)
    add_custom_command(TARGET clay_compiler POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:SDL2::SDL2>
            $<TARGET_FILE_DIR:clay_compiler>
        COMMENT "Copying SDL2 DLL")
endif()

# Test runner
add_executable(test_runner test/test_runtime.c test/test_lexer.c test/test_parser.c)
target_link_libraries(test_runner clay_compiler_lib testy)
# Increase stack size — vm_eval allocates ~400KB per call (BcCompiler + VM stack)
if(MSVC)
    target_link_options(test_runner PRIVATE /STACK:4194304)
else()
    if(APPLE)
        target_link_options(test_runner PRIVATE -Wl,-stack_size,0x400000)
    else()
        target_link_options(test_runner PRIVATE -Wl,-z,stacksize=4194304)
    endif()
endif()

# Copy SDL2 DLL for test runner
if(WIN32 AND SDL2_FOUND AND TARGET SDL2::SDL2)
    add_custom_command(TARGET test_runner POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:SDL2::SDL2>
            $<TARGET_FILE_DIR:test_runner>
        COMMENT "Copying SDL2 DLL for tests")
endif()

enable_testing()
add_test(NAME clay_tests COMMAND test_runner)

# Clay language integration tests (run via bytecode VM)
add_test(NAME clay_test_numbers  COMMAND clay_compiler ${CMAKE_CURRENT_SOURCE_DIR}/test/programs/test_numbers.clay)
add_test(NAME clay_test_floats  COMMAND clay_compiler ${CMAKE_CURRENT_SOURCE_DIR}/test/programs/test_floats.clay)
add_test(NAME clay_test_strings  COMMAND clay_compiler ${CMAKE_CURRENT_SOURCE_DIR}/test/programs/test_strings.clay)
add_test(NAME clay_test_arrays   COMMAND clay_compiler ${CMAKE_CURRENT_SOURCE_DIR}/test/programs/test_arrays.clay)
add_test(NAME clay_test_blocks   COMMAND clay_compiler ${CMAKE_CURRENT_SOURCE_DIR}/test/programs/test_blocks.clay)
add_test(NAME clay_test_objects  COMMAND clay_compiler ${CMAKE_CURRENT_SOURCE_DIR}/test/programs/test_objects.clay)
add_test(NAME clay_test_control  COMMAND clay_compiler ${CMAKE_CURRENT_SOURCE_DIR}/test/programs/test_control.clay)
add_test(NAME clay_test_errors   COMMAND clay_compiler ${CMAKE_CURRENT_SOURCE_DIR}/test/programs/test_errors.clay)
add_test(NAME clay_test_super   COMMAND clay_compiler ${CMAKE_CURRENT_SOURCE_DIR}/test/programs/test_super.clay)
add_test(NAME clay_test_clone_syntax COMMAND clay_compiler ${CMAKE_CURRENT_SOURCE_DIR}/test/programs/test_clone_syntax.clay)
add_test(NAME clay_test_assign_return COMMAND clay_compiler ${CMAKE_CURRENT_SOURCE_DIR}/test/programs/test_assign_return.clay)
add_test(NAME clay_test_reflection COMMAND clay_compiler ${CMAKE_CURRENT_SOURCE_DIR}/test/programs/test_reflection.clay)
add_test(NAME clay_test_array_utils COMMAND clay_compiler ${CMAKE_CURRENT_SOURCE_DIR}/test/programs/test_array_utils.clay)
add_test(NAME clay_test_monorepo_import COMMAND clay_compiler ${CMAKE_CURRENT_SOURCE_DIR}/test/mock_monorepo/test_monorepo.clay)
set_tests_properties(clay_test_monorepo_import PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/test/mock_monorepo)
add_test(NAME clay_test_memory COMMAND clay_compiler ${CMAKE_CURRENT_SOURCE_DIR}/test/programs/test_memory.clay)
add_test(NAME clay_test_file_ops COMMAND clay_compiler ${CMAKE_CURRENT_SOURCE_DIR}/test/programs/test_file_ops.clay)
add_test(NAME clay_test_system COMMAND clay_compiler ${CMAKE_CURRENT_SOURCE_DIR}/test/programs/test_system.clay)
add_test(NAME clay_test_date COMMAND clay_compiler ${CMAKE_CURRENT_SOURCE_DIR}/test/programs/test_date.clay)
add_test(NAME clay_test_http COMMAND clay_compiler ${CMAKE_CURRENT_SOURCE_DIR}/test/programs/test_http.clay)
add_test(NAME clay_test_regex COMMAND clay_compiler ${CMAKE_CURRENT_SOURCE_DIR}/test/programs/test_regex.clay)
add_test(NAME clay_test_crypto COMMAND clay_compiler ${CMAKE_CURRENT_SOURCE_DIR}/test/programs/test_crypto.clay)
add_test(NAME clay_test_toml COMMAND clay_compiler ${CMAKE_CURRENT_SOURCE_DIR}/test/programs/test_toml.clay)
add_test(NAME clay_test_semver_diff COMMAND clay_compiler ${CMAKE_CURRENT_SOURCE_DIR}/test/programs/test_semver_diff.clay)
add_test(NAME clay_test_slot_globals COMMAND clay_compiler ${CMAKE_CURRENT_SOURCE_DIR}/test/programs/test_slot_globals.clay)
add_test(NAME clay_test_dict_order COMMAND clay_compiler ${CMAKE_CURRENT_SOURCE_DIR}/test/programs/test_dict_order.clay)
add_test(NAME clay_test_fs COMMAND clay_compiler ${CMAKE_CURRENT_SOURCE_DIR}/test/programs/test_fs.clay)
add_test(NAME clay_test_process COMMAND clay_compiler ${CMAKE_CURRENT_SOURCE_DIR}/test/programs/test_process.clay)
add_test(NAME clay_test_fiber COMMAND clay_compiler ${CMAKE_CURRENT_SOURCE_DIR}/test/programs/test_fiber.clay)
add_test(NAME clay_test_new_methods COMMAND clay_compiler ${CMAKE_CURRENT_SOURCE_DIR}/test/programs/test_new_methods.clay)
add_test(NAME clay_test_trailing_blocks COMMAND clay_compiler ${CMAKE_CURRENT_SOURCE_DIR}/test/programs/test_trailing_blocks.clay)
add_test(NAME clay_test_return COMMAND clay_compiler ${CMAKE_CURRENT_SOURCE_DIR}/test/programs/test_return.clay)
add_test(NAME clay_test_init_globals COMMAND clay_compiler ${CMAKE_CURRENT_SOURCE_DIR}/test/programs/test_init_globals.clay)
add_test(NAME clay_test_c_names COMMAND clay_compiler ${CMAKE_CURRENT_SOURCE_DIR}/test/programs/test_c_names.clay)
add_test(NAME clay_test_slot_shadowing COMMAND clay_compiler ${CMAKE_CURRENT_SOURCE_DIR}/test/programs/test_slot_shadowing.clay)
add_test(NAME clay_test_scoping COMMAND clay_compiler ${CMAKE_CURRENT_SOURCE_DIR}/test/programs/test_scoping.clay)
add_test(NAME clay_test_break_continue COMMAND clay_compiler ${CMAKE_CURRENT_SOURCE_DIR}/test/programs/test_break_continue.clay)
add_test(NAME clay_test_dict_literal COMMAND clay_compiler ${CMAKE_CURRENT_SOURCE_DIR}/test/programs/test_dict_literal.clay)
add_test(NAME clay_test_null_coalesce COMMAND clay_compiler ${CMAKE_CURRENT_SOURCE_DIR}/test/programs/test_null_coalesce.clay)
add_test(NAME clay_test_default_params COMMAND clay_compiler ${CMAKE_CURRENT_SOURCE_DIR}/test/programs/test_default_params.clay)
add_test(NAME clay_test_slot_assign COMMAND clay_compiler ${CMAKE_CURRENT_SOURCE_DIR}/test/programs/test_slot_assign.clay)
add_test(NAME clay_test_refcount COMMAND clay_compiler ${CMAKE_CURRENT_SOURCE_DIR}/test/programs/test_refcount.clay)
add_test(NAME clay_test_multiline_string COMMAND clay_compiler ${CMAKE_CURRENT_SOURCE_DIR}/test/programs/test_multiline_string.clay)
add_test(NAME clay_test_range COMMAND clay_compiler ${CMAKE_CURRENT_SOURCE_DIR}/test/programs/test_range.clay)
add_test(NAME clay_test_shared_capture COMMAND clay_compiler ${CMAKE_CURRENT_SOURCE_DIR}/test/programs/test_shared_capture.clay)
add_test(NAME clay_test_range_slice COMMAND clay_compiler ${CMAKE_CURRENT_SOURCE_DIR}/test/programs/test_range_slice.clay)
add_test(NAME clay_test_enum COMMAND clay_compiler ${CMAKE_CURRENT_SOURCE_DIR}/test/programs/test_enum.clay)
add_test(NAME clay_test_op_overload COMMAND clay_compiler ${CMAKE_CURRENT_SOURCE_DIR}/test/programs/test_op_overload.clay)
add_test(NAME clay_test_ns_import COMMAND clay_compiler ${CMAKE_CURRENT_SOURCE_DIR}/test/programs/test_ns_import.clay)
add_test(NAME clay_test_file_scope COMMAND clay_compiler ${CMAKE_CURRENT_SOURCE_DIR}/test/programs/test_file_scope.clay)
add_test(NAME clay_test_destructure COMMAND clay_compiler ${CMAKE_CURRENT_SOURCE_DIR}/test/programs/test_destructure.clay)
add_test(NAME clay_test_base64 COMMAND clay_compiler ${CMAKE_CURRENT_SOURCE_DIR}/test/programs/test_base64.clay)
add_test(NAME clay_test_url COMMAND clay_compiler ${CMAKE_CURRENT_SOURCE_DIR}/test/programs/test_url.clay)
add_test(NAME clay_test_config COMMAND clay_compiler ${CMAKE_CURRENT_SOURCE_DIR}/test/programs/test_config.clay)
add_test(NAME clay_test_env COMMAND clay_compiler ${CMAKE_CURRENT_SOURCE_DIR}/test/programs/test_env.clay)
add_test(NAME clay_test_markdown COMMAND clay_compiler ${CMAKE_CURRENT_SOURCE_DIR}/test/programs/test_markdown.clay)
add_test(NAME clay_test_overflow COMMAND clay_compiler ${CMAKE_CURRENT_SOURCE_DIR}/test/programs/test_overflow.clay)
set_tests_properties(clay_test_base64 clay_test_url clay_test_config clay_test_env clay_test_markdown
    PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
               LABELS "bricks")

# The brick registry: `clay bricks search` / `info` against a local index.json
# (CLAY_REGISTRY), which holds entries that must be shown and ones that must not
add_test(NAME clay_bricks_search COMMAND clay_compiler bricks search neural)
set_tests_properties(clay_bricks_search PROPERTIES
    ENVIRONMENT "CLAY_REGISTRY=${CMAKE_CURRENT_SOURCE_DIR}/test/registry/index.json"
    PASS_REGULAR_EXPRESSION "ann  \\(native.*Neural networks over libann.*https://github.com/mseminatore/clay-ann"
    FAIL_REGULAR_EXPRESSION "plain;evil;sneaky" LABELS "bricks")
add_test(NAME clay_bricks_search_all COMMAND clay_compiler bricks search)
set_tests_properties(clay_bricks_search_all PROPERTIES
    ENVIRONMENT "CLAY_REGISTRY=${CMAKE_CURRENT_SOURCE_DIR}/test/registry/index.json"
    PASS_REGULAR_EXPRESSION "ann.*plain.*codeberg.org/someone/clay-plain.*control \\[31mchars"
    FAIL_REGULAR_EXPRESSION "evil.example;Bad Name;unsafe name;\\x1b" LABELS "bricks")
add_test(NAME clay_bricks_info COMMAND clay_compiler bricks info ann)
set_tests_properties(clay_bricks_info PROPERTIES
    ENVIRONMENT "CLAY_REGISTRY=${CMAKE_CURRENT_SOURCE_DIR}/test/registry/index.json"
    PASS_REGULAR_EXPRESSION "version:    0.2.0.*tags:       machine-learning, native.*native:     yes.*clay bricks add github.com/mseminatore/clay-ann ann"
    LABELS "bricks")
add_test(NAME clay_bricks_info_hidden COMMAND clay_compiler bricks info evil)
set_tests_properties(clay_bricks_info_hidden PROPERTIES
    ENVIRONMENT "CLAY_REGISTRY=${CMAKE_CURRENT_SOURCE_DIR}/test/registry/index.json"
    PASS_REGULAR_EXPRESSION "'evil' is not in the brick index" LABELS "bricks")

# Native-backend tests: every program above also compiled with `clay --compile`
# and run (label "native"). Programs that read files relative to the repo root
# run from there; the rest from test/programs (for relative imports).
file(GLOB CLAY_NATIVE_TEST_PROGRAMS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}/test/programs
     ${CMAKE_CURRENT_SOURCE_DIR}/test/programs/test_*.clay)
list(APPEND CLAY_NATIVE_TEST_PROGRAMS
     blocks.clay closures.clay compound_ops.clay dict.clay errors.clay hello.clay implicit_self.clay
     import_test.clay interp.clay logic.clay point.clay proto_chain.clay self.clay
     stack_trace.clay stack_trace2.clay)
set(CLAY_ROOT_CWD_TESTS test_base64 test_url test_config test_env test_markdown)
foreach(prog ${CLAY_NATIVE_TEST_PROGRAMS})
    get_filename_component(name ${prog} NAME_WE)
    # golden-output programs build from a relative path (they run in
    # test/programs), so file names in their output match the VM run's
    set(golden "")
    set(src ${CMAKE_CURRENT_SOURCE_DIR}/test/programs/${prog})
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test/programs/expected/${name}.out)
        set(golden -DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/test/programs/expected/${name}.out)
        set(src ${prog})
    endif()
    add_test(NAME clay_native_${name}
             COMMAND ${CMAKE_COMMAND}
                 -DCLAY=$<TARGET_FILE:clay_compiler>
                 -DSRC=${src}
                 -DOUT=${CMAKE_CURRENT_BINARY_DIR}/native_tests/${name}${CMAKE_EXECUTABLE_SUFFIX}
                 ${golden}
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/native_test.cmake)
    if(name IN_LIST CLAY_ROOT_CWD_TESTS)
        set_tests_properties(clay_native_${name} PROPERTIES
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} LABELS "native;bricks")
    else()
        set_tests_properties(clay_native_${name} PROPERTIES
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/test/programs LABELS "native")
    endif()
endforeach()
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/native_tests)

# Self-hosting: the compiler pieces written in Clay (selfhost/) must agree
# exactly with the C implementation on every .clay file — the lexer with
# `clay --tokens`, the parser with `clay --ast`, the code generator with
# `clay -c`, the bytecode compiler with `clay --bytecode` — in the VM and
# compiled natively. Run from the source root so brick imports resolve.
foreach(check lexer parser codegen bytecode)
    add_test(NAME clay_selfhost_${check}
             COMMAND ${CMAKE_COMMAND}
                 -DCLAY=$<TARGET_FILE:clay_compiler>
                 -DROOT=${CMAKE_CURRENT_SOURCE_DIR} -DCHECK=${check}
                 -DWORK=${CMAKE_CURRENT_BINARY_DIR}/selfhost_${check}
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/selfhost_compare.cmake)
    add_test(NAME clay_selfhost_${check}_native
             COMMAND ${CMAKE_COMMAND}
                 -DCLAY=$<TARGET_FILE:clay_compiler>
                 -DROOT=${CMAKE_CURRENT_SOURCE_DIR} -DCHECK=${check}
                 -DWORK=${CMAKE_CURRENT_BINARY_DIR}/selfhost_${check}_native
                 -DNATIVE=${CMAKE_CURRENT_BINARY_DIR}/native_tests/clay_selfhost_${check}${CMAKE_EXECUTABLE_SUFFIX}
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/selfhost_compare.cmake)
    set_tests_properties(clay_selfhost_${check} PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} LABELS "selfhost")
    set_tests_properties(clay_selfhost_${check}_native PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} LABELS "selfhost;native")
endforeach()
# clayc runs every test program on the VM exactly like clay does
add_test(NAME clay_selfhost_run
         COMMAND ${CMAKE_COMMAND}
             -DCLAY=$<TARGET_FILE:clay_compiler>
             -DROOT=${CMAKE_CURRENT_SOURCE_DIR}
             -DCLAYC=${CMAKE_CURRENT_BINARY_DIR}/native_tests/clayc_run${CMAKE_EXECUTABLE_SUFFIX}
             -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/selfhost_run.cmake)
set_tests_properties(clay_selfhost_run PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} LABELS "selfhost;native")
# clayc bricks (selfhost/clay-bricks) does what clay bricks does, against git
# repositories made by the test (no network)
add_test(NAME clay_selfhost_bricks
         COMMAND ${CMAKE_COMMAND}
             -DCLAY=$<TARGET_FILE:clay_compiler>
             -DROOT=${CMAKE_CURRENT_SOURCE_DIR}
             -DWORK=${CMAKE_CURRENT_BINARY_DIR}/selfhost_bricks
             -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/selfhost_bricks.cmake)
set_tests_properties(clay_selfhost_bricks PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} LABELS "selfhost")
# Bootstrap: clayc (selfhost/clayc.clay) compiles itself to a fixpoint. Needs
# a cc-style clay-native.cfg (clayc doesn't drive MSVC yet).
if(NOT MSVC)
    add_test(NAME clay_selfhost_bootstrap
             COMMAND ${CMAKE_COMMAND}
                 -DCLAY=$<TARGET_FILE:clay_compiler>
                 -DROOT=${CMAKE_CURRENT_SOURCE_DIR}
                 -DWORK=${CMAKE_CURRENT_BINARY_DIR}/selfhost_bootstrap
                 -DCFG=$<TARGET_FILE_DIR:clay_compiler>/clay-native.cfg
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/selfhost_bootstrap.cmake)
    set_tests_properties(clay_selfhost_bootstrap PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} LABELS "selfhost;native")
endif()

# Documentation tests: every ```clay block in the guide (docs/*.md) and README
# must run without error (blocks marked ```clay norun are skipped)
add_executable(doc_test tools/doc_test.c)
target_compile_definitions(doc_test PRIVATE _CRT_SECURE_NO_WARNINGS)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/doc_tests)
foreach(doc getting-started language-tour core-objects native-and-plugins bricks debugging limitations)
    add_test(NAME clay_docs_${doc}
             COMMAND doc_test $<TARGET_FILE:clay_compiler>
                     ${CMAKE_CURRENT_SOURCE_DIR}/docs/${doc}.md ${CMAKE_CURRENT_BINARY_DIR}/doc_tests)
    set_tests_properties(clay_docs_${doc} PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/doc_tests LABELS "docs")
endforeach()
add_test(NAME clay_docs_readme
         COMMAND doc_test $<TARGET_FILE:clay_compiler>
                 ${CMAKE_CURRENT_SOURCE_DIR}/README.md ${CMAKE_CURRENT_BINARY_DIR}/doc_tests)
set_tests_properties(clay_docs_readme PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/doc_tests LABELS "docs")

# Golden-output tests: demo programs without assertions; each must print
# exactly test/programs/expected/<name>.out (native builds are checked against
# the same file). Not tests: bench_dispatch (benchmark), dap_test (debugger
# fixture), math/math_lib (libraries imported by import_test).
set(CLAY_GOLDEN_TESTS blocks closures compound_ops dict errors hello implicit_self
    import_test interp logic point proto_chain self stack_trace2 test_db_fix)
foreach(smoke ${CLAY_GOLDEN_TESTS})
    add_test(NAME clay_smoke_${smoke}
             COMMAND ${CMAKE_COMMAND}
                 -DCLAY=$<TARGET_FILE:clay_compiler>
                 -DSRC=${smoke}.clay
                 -DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/test/programs/expected/${smoke}.out
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/expect_output.cmake)
    set_tests_properties(clay_smoke_${smoke} PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/test/programs
        LABELS "smoke")
endforeach()
# A native brick (test/native_brick/bricks/stats): its C is built into a
# plugin on first use for the VM, and linked into the --compile executable
add_test(NAME clay_native_brick COMMAND clay_compiler test_native_brick.clay)
add_test(NAME clay_native_brick_compiled
         COMMAND ${CMAKE_COMMAND} -DCLAY=$<TARGET_FILE:clay_compiler>
             -DSRC=test_native_brick.clay
             -DOUT=${CMAKE_CURRENT_BINARY_DIR}/native_tests/native_brick${CMAKE_EXECUTABLE_SUFFIX}
             -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/native_test.cmake)
set_tests_properties(clay_native_brick clay_native_brick_compiled PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/test/native_brick
    RESOURCE_LOCK native_brick_build)

# Compile errors: each test/errors/<name>.clay must fail with exactly
# test/errors/<name>.out, in the VM and with --compile (clayc is checked
# against clay in cmake/selfhost_run.cmake)
foreach(err undefined_variable undefined_in_import)
    add_test(NAME clay_error_${err}
             COMMAND ${CMAKE_COMMAND} -DCLAY=$<TARGET_FILE:clay_compiler>
                 -DSRC=test/errors/${err}.clay -DEXPECT_FAIL=1
                 -DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/test/errors/${err}.out
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/expect_output.cmake)
    add_test(NAME clay_error_${err}_native
             COMMAND ${CMAKE_COMMAND} -DCLAY=$<TARGET_FILE:clay_compiler>
                 -DSRC=test/errors/${err}.clay -DEXPECT_FAIL=1
                 "-DFLAGS=--compile;-o;${CMAKE_CURRENT_BINARY_DIR}/native_tests/err_${err}"
                 -DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/test/errors/${err}.out
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/expect_output.cmake)
    set_tests_properties(clay_error_${err} clay_error_${err}_native PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endforeach()

# stack_trace asserts on its own trace (the frames' wording differs by backend)
add_test(NAME clay_smoke_stack_trace COMMAND clay_compiler ${CMAKE_CURRENT_SOURCE_DIR}/test/programs/stack_trace.clay)
set_tests_properties(clay_smoke_stack_trace PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/test/programs LABELS "smoke")

# Example plugin — mathex
add_library(mathex MODULE examples/plugins/example_plugin.c)
target_include_directories(mathex PRIVATE src/runtime)
set_target_properties(mathex PROPERTIES
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugins
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugins)
# On Windows, MODULE produces .dll in RUNTIME_OUTPUT_DIRECTORY
if(WIN32)
    set_target_properties(mathex PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/Debug/plugins
        LIBRARY_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/Release/plugins
        RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/Debug/plugins
        RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/Release/plugins)
endif()
