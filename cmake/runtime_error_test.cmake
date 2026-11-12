# runtime_error_test.cmake — a program that ends with an uncaught error must
# print exactly the golden file (the error and its traceback), in the VM and,
# with -DNATIVE_OUT=<binary>, as a native executable too. Run from the source
# root so the file names in the output are the same everywhere.
#
#   cmake -DCLAY=<clay binary> -DSRC=<program.clay> -DEXPECTED=<file.out>
#         [-DNATIVE_OUT=<binary to build>] -P runtime_error_test.cmake
#
# To update a golden file after an intended change:
#   build/clay test/errors/<name>.clay > test/errors/<name>.out 2>&1

foreach(var CLAY SRC EXPECTED)
    if(NOT ${var})
        message(FATAL_ERROR "runtime_error_test.cmake: ${var} is not set")
    endif()
endforeach()

file(READ "${EXPECTED}" want)
string(REPLACE "\r\n" "\n" want "${want}")

function(check_output what rc out)
    if(rc EQUAL 0)
        message(FATAL_ERROR "${what}: expected an error, but it exited with 0:\n${out}")
    endif()
    string(REPLACE "\r\n" "\n" got "${out}")
    # AddressSanitizer builds print a notice when an error unwinds a fiber's stack
    string(REGEX REPLACE "==[0-9]+==WARNING: ASan is ignoring[^\n]*\n" "" got "${got}")
    string(REGEX REPLACE "False positive error reports may follow\n" "" got "${got}")
    string(REGEX REPLACE "For details see https://github.com/google/sanitizers/issues/189\n" "" got "${got}")
    if(NOT got STREQUAL want)
        message(FATAL_ERROR "${what}: output differs from ${EXPECTED}\n--- expected ---\n${want}--- got ---\n${got}")
    endif()
endfunction()

if(NATIVE_OUT)
    execute_process(COMMAND "${CLAY}" --compile -o "${NATIVE_OUT}" "${SRC}"
                    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "clay --compile failed (${rc}):\n${out}")
    endif()
    execute_process(COMMAND "${NATIVE_OUT}" RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
    check_output("native" "${rc}" "${out}")
else()
    execute_process(COMMAND "${CLAY}" "${SRC}" RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
    check_output("VM" "${rc}" "${out}")
endif()
