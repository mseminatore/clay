# native_test.cmake — compile a .clay program with `clay --compile` and run it.
#
#   cmake -DCLAY=<clay binary> -DSRC=<program.clay> -DOUT=<binary to build>
#         [-DEXPECTED=<golden output> [-DEXPECT_FAIL=1]] -P native_test.cmake
#
# Passes when the program compiles and exits with status 0 (test programs
# raise — and so exit non-zero — on any failed assertion).

foreach(var CLAY SRC OUT)
    if(NOT ${var})
        message(FATAL_ERROR "native_test.cmake: ${var} is not set")
    endif()
endforeach()

execute_process(
    COMMAND "${CLAY}" --compile -o "${OUT}" "${SRC}"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "clay --compile failed (${rc}):\n${out}\n${err}")
endif()

# With -DEXPECTED=<file.out>, its output must also match the golden file
# (the same one the VM run is checked against); with -DEXPECT_FAIL=1 as well,
# the program must exit non-zero
if(EXPECTED)
    execute_process(COMMAND "${OUT}" RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
    include("${CMAKE_CURRENT_LIST_DIR}/expect_compare.cmake")
    if(EXPECT_FAIL)
        if(rc EQUAL 0)
            message(FATAL_ERROR "expected ${OUT} to fail, but it exited with 0:\n${out}")
        endif()
        set(rc 0)
    endif()
    expect_compare("${rc}" "${out}" "${EXPECTED}")
    return()
endif()

execute_process(
    COMMAND "${OUT}"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "native program exited with ${rc}:\n${out}\n${err}")
endif()
message(STATUS "${out}")
