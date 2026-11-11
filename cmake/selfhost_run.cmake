# selfhost_run.cmake — clayc (selfhost/clayc.clay, built natively) runs
# programs on the VM with bytecode from the Clay compiler; every program in
# test/programs must behave exactly as under `clay` (stdout, stderr, exit
# status). Programs whose bricks aren't installed (`clay bricks install`;
# CTest label "bricks") are skipped. Compile errors and `clayc bricks` are
# compared with clay's too.
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
set(skipped "")
foreach(f ${files})
    execute_process(COMMAND "${CLAY}" "${f}" WORKING_DIRECTORY "${dir}" TIMEOUT 120
                    RESULT_VARIABLE rc_a OUTPUT_VARIABLE out_a ERROR_VARIABLE err_a)
    if(err_a MATCHES "cannot open import '[^']*/" OR out_a MATCHES "cannot open import '[^']*/")
        list(APPEND skipped "${f}")   # a brick that isn't installed
        continue()
    endif()
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

# A native brick (test/native_brick): clayc builds/loads its plugin too
set(nb "${ROOT}/test/native_brick")
execute_process(COMMAND "${CLAY}" test_native_brick.clay WORKING_DIRECTORY "${nb}" TIMEOUT 300
                RESULT_VARIABLE rc_a OUTPUT_VARIABLE out_a ERROR_VARIABLE err_a)
execute_process(COMMAND "${CLAYC}" test_native_brick.clay WORKING_DIRECTORY "${nb}" TIMEOUT 300
                RESULT_VARIABLE rc_b OUTPUT_VARIABLE out_b ERROR_VARIABLE err_b)
# (a "building native brick" line may come from whichever ran first)
string(REGEX REPLACE "clay: building native brick[^\n]*\n" "" err_a "${err_a}")
string(REGEX REPLACE "clay: building native brick[^\n]*\n" "" err_b "${err_b}")
if(NOT rc_a STREQUAL rc_b OR NOT out_a STREQUAL out_b OR NOT err_a STREQUAL err_b)
    list(APPEND failed "test/native_brick/test_native_brick.clay")
    message("--- native brick: clay (exit ${rc_a}) ---\n${out_a}${err_a}--- clayc (exit ${rc_b}) ---\n${out_b}${err_b}")
endif()

# Compile errors (test/errors/*.clay, helpers excluded): clayc must report
# the same as clay, apart from its own name in the final line
file(GLOB errfiles RELATIVE "${ROOT}" "${ROOT}/test/errors/*.clay")
foreach(f ${errfiles})
    if(f MATCHES "/helper_")
        continue()
    endif()
    execute_process(COMMAND "${CLAY}" "${f}" WORKING_DIRECTORY "${ROOT}" TIMEOUT 120
                    RESULT_VARIABLE rc_a OUTPUT_VARIABLE out_a ERROR_VARIABLE out_a)
    execute_process(COMMAND "${CLAYC}" "${f}" WORKING_DIRECTORY "${ROOT}" TIMEOUT 120
                    RESULT_VARIABLE rc_b OUTPUT_VARIABLE out_b ERROR_VARIABLE out_b)
    string(REPLACE "clayc: " "clay: " out_b "${out_b}")
    if(NOT rc_a STREQUAL rc_b OR NOT out_a STREQUAL out_b)
        list(APPEND failed "${f}")
        message("--- ${f}: clay (exit ${rc_a}) ---\n${out_a}--- clayc (exit ${rc_b}) ---\n${out_b}")
    endif()
endforeach()

# The package manager: `clayc bricks` (selfhost/clay-bricks) answers like
# `clay bricks`, here for the commands that need neither git nor the network
# (cmake/selfhost_bricks.cmake runs the rest, with clayc on the VM)
get_filename_component(bw "${CLAYC}" DIRECTORY)
set(bw "${bw}/clayc_bricks")
file(REMOVE_RECURSE "${bw}")
file(MAKE_DIRECTORY "${bw}/clay/proj" "${bw}/clayc/proj")
file(WRITE "${bw}/empty" "")
set(ENV{CLAY_REGISTRY} "${ROOT}/test/registry/index.json")
foreach(cmd "" "bogus" "info" "add" "remove" "search" "search neural" "info ann" "info evil"
            "list" "init" "init" "list" "install" "update" "remove nope")
    separate_arguments(args UNIX_COMMAND "${cmd}")
    execute_process(COMMAND "${CLAY}" bricks ${args}
                    WORKING_DIRECTORY "${bw}/clay/proj" INPUT_FILE "${bw}/empty" TIMEOUT 120
                    RESULT_VARIABLE rc_a OUTPUT_VARIABLE out_a ERROR_VARIABLE out_a)
    execute_process(COMMAND "${CLAYC}" bricks ${args}
                    WORKING_DIRECTORY "${bw}/clayc/proj" INPUT_FILE "${bw}/empty" TIMEOUT 120
                    RESULT_VARIABLE rc_b OUTPUT_VARIABLE out_b ERROR_VARIABLE out_b)
    string(REPLACE "\r" "" out_a "${out_a}")
    string(REPLACE "\r" "" out_b "${out_b}")
    string(REPLACE "clayc bricks" "clay bricks" out_b "${out_b}")
    if(NOT rc_a STREQUAL rc_b OR NOT out_a STREQUAL out_b)
        list(APPEND failed "bricks ${cmd}")
        message("--- bricks ${cmd}: clay (exit ${rc_a}) ---\n${out_a}--- clayc (exit ${rc_b}) ---\n${out_b}")
    endif()
endforeach()
unset(ENV{CLAY_REGISTRY})

list(LENGTH files count)
list(LENGTH skipped nskip)
if(nskip GREATER 0)
    string(REPLACE ";" " " list "${skipped}")
    message(STATUS "skipped ${nskip} programs whose bricks aren't installed: ${list}")
endif()
if(failed)
    list(LENGTH failed nfail)
    string(REPLACE ";" "\n  " list "${failed}")
    message(FATAL_ERROR "${nfail} of ${count} programs behave differently under clayc:\n  ${list}")
endif()
math(EXPR ran "${count} - ${nskip}")
message(STATUS "${ran} programs run identically under clay and clayc")
