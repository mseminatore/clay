# selfhost_compare.cmake — the self-hosted compiler pieces (written in Clay)
# must agree exactly with the C implementation on every .clay file in the
# repository and in their edge-case corpora.
#
#   cmake -DCLAY=<clay binary> -DROOT=<source dir> -DCHECK=lexer|parser
#         [-DNATIVE=<binary to build>] -P selfhost_compare.cmake
#
#   lexer:  `clay --tokens f`  vs  selfhost/clay-lexer/tools/tokens.clay f
#   parser: `clay --ast f`     vs  selfhost/clay-parser/tools/ast.clay f
#           (stdout and stderr together, and the exit status: syntax errors
#           must match too)
#
# Without NATIVE the Clay side runs in the VM; with NATIVE its tool is first
# compiled with `clay --compile -o <NATIVE>` and that binary is used.

foreach(var CLAY ROOT CHECK)
    if(NOT ${var})
        message(FATAL_ERROR "selfhost_compare.cmake: ${var} is not set")
    endif()
endforeach()

if(CHECK STREQUAL "lexer")
    set(flag --tokens)
    set(tool "${ROOT}/selfhost/clay-lexer/tools/tokens.clay")
elseif(CHECK STREQUAL "parser")
    set(flag --ast)
    set(tool "${ROOT}/selfhost/clay-parser/tools/ast.clay")
else()
    message(FATAL_ERROR "selfhost_compare.cmake: CHECK must be lexer or parser")
endif()

if(NATIVE)
    execute_process(
        COMMAND "${CLAY}" --compile -o "${NATIVE}" "${tool}"
        RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "clay --compile ${tool} failed (${rc}):\n${out}\n${err}")
    endif()
    set(clay_side "${NATIVE}")
else()
    set(clay_side "${CLAY};${tool}")
endif()

file(GLOB_RECURSE files
     "${ROOT}/test/*.clay" "${ROOT}/examples/*.clay" "${ROOT}/selfhost/*.clay"
     "${ROOT}/tools/*.clay" "${ROOT}/docs/*.clay" "${ROOT}/bricks/*.clay")
list(SORT files)
list(LENGTH files count)
if(count LESS 50)
    message(FATAL_ERROR "selfhost_compare.cmake: only ${count} .clay files found under ${ROOT}")
endif()

set(failed "")
foreach(f ${files})
    execute_process(COMMAND "${CLAY}" ${flag} "${f}"
                    RESULT_VARIABLE rc_c OUTPUT_VARIABLE want ERROR_VARIABLE want)
    execute_process(COMMAND ${clay_side} "${f}"
                    RESULT_VARIABLE rc_clay OUTPUT_VARIABLE got ERROR_VARIABLE got)
    if(NOT rc_c EQUAL rc_clay OR NOT want STREQUAL got)
        file(RELATIVE_PATH rel "${ROOT}" "${f}")
        list(APPEND failed "${rel}")
        list(LENGTH failed nfail)
        if(nfail LESS_EQUAL 3)
            message("--- ${rel}: C (exit ${rc_c}) ---\n${want}"
                    "--- Clay (exit ${rc_clay}) ---\n${got}")
        endif()
    endif()
endforeach()

if(failed)
    list(LENGTH failed nfail)
    string(REPLACE ";" "\n  " list "${failed}")
    message(FATAL_ERROR "${CHECK}: ${nfail} of ${count} files differ:\n  ${list}")
endif()
message(STATUS "${CHECK}: ${count} files, the Clay ${CHECK} matches the C ${CHECK}")
