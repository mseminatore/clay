# selfhost_run.cmake — clayc (selfhost/clayc.clay, built natively) runs
# programs on the VM with bytecode from the Clay compiler; every program in
# test/programs must behave exactly as under `clay` (stdout, stderr, exit
# status).
#
#   cmake -DCLAY=<clay binary> -DROOT=<source dir> -DCLAYC=<clayc to build>
#         -P selfhost_run.cmake

foreach(var CLAY ROOT CLAYC)
    if(NOT ${var})
        message(FATAL_ERROR "selfhost_run.cmake: ${var} is not set")
    endif()
endforeach()

execute_process(COMMAND "${CLAY}" --compile -o "${CLAYC}" "${ROOT}/selfhost/clayc.clay"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "clay --compile clayc failed (${rc}):\n${out}\n${err}")
endif()

set(dir "${ROOT}/test/programs")
file(GLOB files RELATIVE "${dir}" "${dir}/*.clay")
list(SORT files)
set(failed "")
foreach(f ${files})
    execute_process(COMMAND "${CLAY}" "${f}" WORKING_DIRECTORY "${dir}" TIMEOUT 120
                    RESULT_VARIABLE rc_a OUTPUT_VARIABLE out_a ERROR_VARIABLE err_a)
    execute_process(COMMAND "${CLAYC}" "${f}" WORKING_DIRECTORY "${dir}" TIMEOUT 120
                    RESULT_VARIABLE rc_b OUTPUT_VARIABLE out_b ERROR_VARIABLE err_b)
    if(NOT rc_a STREQUAL rc_b OR NOT out_a STREQUAL out_b OR NOT err_a STREQUAL err_b)
        list(APPEND failed "${f}")
        list(LENGTH failed nfail)
        if(nfail LESS_EQUAL 3)
            message("--- ${f}: clay (exit ${rc_a}) ---\n${out_a}${err_a}"
                    "--- clayc (exit ${rc_b}) ---\n${out_b}${err_b}")
        endif()
    endif()
endforeach()

list(LENGTH files count)
if(failed)
    list(LENGTH failed nfail)
    string(REPLACE ";" "\n  " list "${failed}")
    message(FATAL_ERROR "${nfail} of ${count} programs behave differently under clayc:\n  ${list}")
endif()
message(STATUS "${count} programs run identically under clay and clayc")
