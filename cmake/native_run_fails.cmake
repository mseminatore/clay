# native_run_fails.cmake — compile a program with `clay --compile`, run it, and
# pass when it ends with the error and traceback of a runaway recursion.
#
#   cmake -DCLAY=<clay binary> -DSRC=<program.clay> -DOUT=<binary to build>
#         -P native_run_fails.cmake

foreach(var CLAY SRC OUT)
    if(NOT ${var})
        message(FATAL_ERROR "native_run_fails.cmake: ${var} is not set")
    endif()
endforeach()

execute_process(COMMAND "${CLAY}" --compile -o "${OUT}" "${SRC}"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "clay --compile failed (${rc}):\n${out}")
endif()
execute_process(COMMAND "${OUT}" RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
if(rc EQUAL 0)
    message(FATAL_ERROR "expected a stack overflow error, but the program exited with 0:\n${out}")
endif()
if(NOT out MATCHES "Error: stack overflow: recursion is too deep" OR
   NOT out MATCHES "previous line repeated [0-9]+ more times")
    message(FATAL_ERROR "unexpected output:\n${out}")
endif()
