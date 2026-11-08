# expect_output.cmake — run a program and compare its output with a golden file.
#
#   cmake -DCLAY=<clay binary> -DSRC=<program.clay> -DEXPECTED=<file.out> -P expect_output.cmake
#
# Passes when the program exits with status 0 and its stdout and stderr
# (merged, in order) equal the expected file exactly. -DEXPECT_FAIL=1 wants a
# non-zero exit instead (compile errors); -DFLAGS="--compile;-o;out" passes
# options to clay before the source. Windows line endings
# are normalized. To update a golden file after an intended change, run the
# program from test/programs and save its output:
#   ../../build/clay hello.clay > expected/hello.out 2>&1

foreach(var CLAY SRC EXPECTED)
    if(NOT ${var})
        message(FATAL_ERROR "expect_output.cmake: ${var} is not set")
    endif()
endforeach()

execute_process(COMMAND "${CLAY}" ${FLAGS} "${SRC}"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
include("${CMAKE_CURRENT_LIST_DIR}/expect_compare.cmake")
if(EXPECT_FAIL)
    if(rc EQUAL 0)
        message(FATAL_ERROR "expected ${SRC} to fail, but it exited with 0:\n${out}")
    endif()
    set(rc 0)
endif()
expect_compare("${rc}" "${out}" "${EXPECTED}")
