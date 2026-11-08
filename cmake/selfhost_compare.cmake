# selfhost_compare.cmake — the self-hosted compiler pieces (written in Clay)
# must agree exactly with the C implementation on every .clay file in the
# repository and in their edge-case corpora.
#
#   cmake -DCLAY=<clay binary> -DROOT=<source dir> -DCHECK=lexer|parser|codegen|bytecode
#         [-DNATIVE=<binary to build>] [-DWORK=<scratch dir>] -P selfhost_compare.cmake
#
#   lexer:   `clay --tokens f`  vs  selfhost/clay-lexer/tools/tokens.clay f
#   parser:  `clay --ast f`     vs  selfhost/clay-parser/tools/ast.clay f
#            (stdout and stderr together, and the exit status: syntax errors
#            must match too)
#   bytecode: `clay --bytecode f` vs selfhost/clay-bytecode/tools/bc.clay f
#            (constants, code, chunks and methods, and the exit status)
#   codegen: `clay -c -o a.c f` vs  selfhost/clay-codegen/tools/c.clay f b.c
#            (the generated C byte for byte, and the exit status; WORK holds
#            the C files)
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
elseif(CHECK STREQUAL "bytecode")
    set(flag --bytecode)
    set(tool "${ROOT}/selfhost/clay-bytecode/tools/bc.clay")
elseif(CHECK STREQUAL "codegen")
    set(tool "${ROOT}/selfhost/clay-codegen/tools/c.clay")
    if(NOT WORK)
        message(FATAL_ERROR "selfhost_compare.cmake: codegen needs WORK")
    endif()
    file(MAKE_DIRECTORY "${WORK}")
else()
    message(FATAL_ERROR "selfhost_compare.cmake: CHECK must be lexer, parser, codegen or bytecode")
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
# native bricks are compiled by clay but not yet by the Clay compiler (clayc)
list(FILTER files EXCLUDE REGEX "/test/native_brick/")
list(SORT files)
list(LENGTH files count)
if(count LESS 50)
    message(FATAL_ERROR "selfhost_compare.cmake: only ${count} .clay files found under ${ROOT}")
endif()

# codegen: the generated C names the clay whose plugins/ native programs load;
# a native tool has no `System executable`, so pass the VM's explicitly
set(plugin_args "")
if(CHECK STREQUAL "codegen" AND NATIVE)
    file(WRITE "${WORK}/exe.clay" "Console print(System executable)\n")
    execute_process(COMMAND "${CLAY}" "${WORK}/exe.clay" OUTPUT_VARIABLE exe)
    set(plugin_args --plugin-exe "${exe}")
endif()

set(failed "")
set(index 0)
foreach(f ${files})
    if(CHECK STREQUAL "codegen")
        math(EXPR index "${index} + 1")
        set(c_out "${WORK}/${index}.c")
        set(clay_out "${WORK}/${index}.clay.c")
        file(REMOVE "${c_out}" "${clay_out}")
        execute_process(COMMAND "${CLAY}" --no-check -c -o "${c_out}" "${f}"
                        RESULT_VARIABLE rc_c OUTPUT_QUIET ERROR_QUIET)
        execute_process(COMMAND ${clay_side} "${f}" "${clay_out}" ${plugin_args}
                        RESULT_VARIABLE rc_clay OUTPUT_VARIABLE got ERROR_VARIABLE got)
        set(want "")
        set(have "")
        if(EXISTS "${c_out}")
            file(READ "${c_out}" want)
        endif()
        if(EXISTS "${clay_out}")
            file(READ "${clay_out}" have)
        endif()
        if(NOT rc_c EQUAL rc_clay OR NOT want STREQUAL have)
            file(RELATIVE_PATH rel "${ROOT}" "${f}")
            list(APPEND failed "${rel}")
            list(LENGTH failed nfail)
            if(nfail LESS_EQUAL 3)
                message("--- ${rel}: C exit ${rc_c}, Clay exit ${rc_clay}: diff ${c_out} ${clay_out}\n${got}")
            endif()
        endif()
        continue()
    endif()
    execute_process(COMMAND "${CLAY}" --no-check ${flag} "${f}"
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
