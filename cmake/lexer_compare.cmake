# lexer_compare.cmake — the Clay lexer (selfhost/clay-lexer, written in Clay)
# must produce exactly the tokens of the C lexer (`clay --tokens`) for every
# .clay file in the repository and in its edge-case corpus.
#
#   cmake -DCLAY=<clay binary> -DROOT=<source dir> [-DNATIVE=<binary to build>]
#         -P lexer_compare.cmake
#
# Without NATIVE the Clay lexer runs in the VM; with NATIVE it is first
# compiled with `clay --compile -o <NATIVE>` and that binary is used.

foreach(var CLAY ROOT)
    if(NOT ${var})
        message(FATAL_ERROR "lexer_compare.cmake: ${var} is not set")
    endif()
endforeach()

set(tool "${ROOT}/selfhost/clay-lexer/tools/tokens.clay")
if(NATIVE)
    execute_process(
        COMMAND "${CLAY}" --compile -o "${NATIVE}" "${tool}"
        RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "clay --compile ${tool} failed (${rc}):\n${out}\n${err}")
    endif()
    set(clay_lexer "${NATIVE}")
else()
    set(clay_lexer "${CLAY};${tool}")
endif()

file(GLOB_RECURSE files
     "${ROOT}/test/*.clay" "${ROOT}/examples/*.clay" "${ROOT}/selfhost/*.clay"
     "${ROOT}/tools/*.clay" "${ROOT}/docs/*.clay" "${ROOT}/bricks/*.clay")
list(SORT files)
list(LENGTH files count)
if(count LESS 50)
    message(FATAL_ERROR "lexer_compare.cmake: only ${count} .clay files found under ${ROOT}")
endif()

set(failed "")
foreach(f ${files})
    execute_process(COMMAND "${CLAY}" --tokens "${f}"
                    RESULT_VARIABLE rc_c OUTPUT_VARIABLE want ERROR_VARIABLE err_c)
    execute_process(COMMAND ${clay_lexer} "${f}"
                    RESULT_VARIABLE rc_clay OUTPUT_VARIABLE got ERROR_VARIABLE err_clay)
    if(NOT rc_c EQUAL 0 OR NOT rc_clay EQUAL 0 OR NOT want STREQUAL got)
        file(RELATIVE_PATH rel "${ROOT}" "${f}")
        list(APPEND failed "${rel}")
        list(LENGTH failed nfail)
        if(nfail LESS_EQUAL 3)
            message("--- ${rel}: C lexer (exit ${rc_c}) ---\n${want}${err_c}"
                    "--- Clay lexer (exit ${rc_clay}) ---\n${got}${err_clay}")
        endif()
    endif()
endforeach()

if(failed)
    list(LENGTH failed nfail)
    string(REPLACE ";" "\n  " list "${failed}")
    message(FATAL_ERROR "${nfail} of ${count} files tokenize differently:\n  ${list}")
endif()
message(STATUS "${count} files: the Clay lexer matches the C lexer")
