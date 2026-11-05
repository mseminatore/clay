# native_test.cmake — compile a .clay program with `clay --compile` and run it.
#
#   cmake -DCLAY=<clay binary> -DSRC=<program.clay> -DOUT=<binary to build> -P native_test.cmake
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

execute_process(
    COMMAND "${OUT}"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "native program exited with ${rc}:\n${out}\n${err}")
endif()
message(STATUS "${out}")
