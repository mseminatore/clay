# selfhost_bootstrap.cmake — Clay compiles itself.
#
#   cmake -DCLAY=<clay binary> -DROOT=<source dir> -DWORK=<scratch dir>
#         -DCFG=<clay-native.cfg> -P selfhost_bootstrap.cmake
#
# 1. clay --compile builds clayc1 from selfhost/clayc.clay (C compiler)
# 2. clayc1 builds clayc2 from the same source (the Clay compiler, native)
# 3. clayc2 builds clayc3
# The C of generations 2 and 3 must be identical (the fixpoint), and equal to
# what the C compiler generated for generation 1. Finally clayc3 compiles a
# test program, which must run successfully.

foreach(var CLAY ROOT WORK CFG)
    if(NOT ${var})
        message(FATAL_ERROR "selfhost_bootstrap.cmake: ${var} is not set")
    endif()
endforeach()

file(MAKE_DIRECTORY "${WORK}")
if(WIN32)
    set(ext ".exe")
else()
    set(ext "")
endif()
set(src "${ROOT}/selfhost/clayc.clay")

function(run_step what)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "${what} failed (${rc}):\n${out}\n${err}")
    endif()
endfunction()

# Every generation names the same clay as the plugin host, so the C matches
file(WRITE "${WORK}/exe.clay" "Console print(System executable)\n")
execute_process(COMMAND "${CLAY}" "${WORK}/exe.clay" OUTPUT_VARIABLE exe)

run_step("stage 1: clay --compile clayc" "${CLAY}" --compile -o "${WORK}/clayc1${ext}" "${src}")
foreach(gen 2 3)
    math(EXPR prev "${gen} - 1")
    run_step("stage ${gen}: clayc${prev} compiling clayc"
             "${WORK}/clayc${prev}${ext}" --cfg "${CFG}" --plugin-exe "${exe}"
             -o "${WORK}/clayc${gen}${ext}" "${src}")
endforeach()

file(READ "${WORK}/clayc1.c" c1)
file(READ "${WORK}/clayc2.c" c2)
file(READ "${WORK}/clayc3.c" c3)
if(NOT c2 STREQUAL c3)
    message(FATAL_ERROR "no fixpoint: clayc2.c and clayc3.c differ (diff ${WORK}/clayc2.c ${WORK}/clayc3.c)")
endif()
if(NOT c1 STREQUAL c2)
    message(FATAL_ERROR "the Clay compiler's C differs from the C compiler's (diff ${WORK}/clayc1.c ${WORK}/clayc2.c)")
endif()

# The third generation compiles a real program
run_step("clayc3 compiling test_return.clay"
         "${WORK}/clayc3${ext}" --cfg "${CFG}" --plugin-exe "${exe}"
         -o "${WORK}/test_return${ext}" "${ROOT}/test/programs/test_return.clay")
execute_process(COMMAND "${WORK}/test_return${ext}" WORKING_DIRECTORY "${ROOT}/test/programs"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "test_return built by clayc3 failed (${rc}):\n${out}\n${err}")
endif()

string(LENGTH "${c3}" size)
message(STATUS "bootstrap: clayc2.c == clayc3.c == clayc1.c (${size} bytes); clayc3 builds working programs")
