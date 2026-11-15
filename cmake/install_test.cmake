# install_test.cmake -- `cmake --install` produces a Clay that works on its own:
# bin/clay plus lib/clay/{stdlib,web}, the bundled bricks found next to the
# executable (CLAY_LIB unset), and no git metadata or build-machine files.
# lib/clay/native lets the installed clay --compile programs and build native
# bricks with no build tree or checkout: its cfg names no path of either.
#
#   cmake -DBUILD=<build dir> -DSOURCE=<source dir> [-DCONFIG=<config>] -DVERSION=<x.y.z>
#         -DWORK=<scratch dir> -P install_test.cmake

foreach(var BUILD SOURCE VERSION WORK)
    if(NOT ${var})
        message(FATAL_ERROR "install_test.cmake: ${var} is not set")
    endif()
endforeach()
unset(ENV{CLAY_LIB})

set(prefix "${WORK}/prefix")
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")

set(cfg_args "")
if(CONFIG)
    set(cfg_args --config "${CONFIG}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --install "${BUILD}" --prefix "${prefix}" ${cfg_args}
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "cmake --install failed (${rc}):\n${out}\n${err}")
endif()

if(CMAKE_HOST_WIN32)
    set(clay "${prefix}/bin/clay.exe")
else()
    set(clay "${prefix}/bin/clay")
endif()

# 1. the expected files are there
foreach(f "${clay}"
          lib/clay/stdlib/brick.json lib/clay/stdlib/src/main.clay lib/clay/stdlib/json/src/main.clay
          lib/clay/web/brick.json lib/clay/web/router/src/main.clay
          share/doc/clay/LICENSE share/doc/clay/README.md share/doc/clay/THIRD_PARTY_NOTICES.md)
    if(IS_ABSOLUTE "${f}")
        set(path "${f}")
    else()
        set(path "${prefix}/${f}")
    endif()
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "not installed: ${f}")
    endif()
endforeach()

# 2. nothing that belongs to the source tree or this build machine
file(GLOB_RECURSE stray LIST_DIRECTORIES true RELATIVE "${prefix}" "${prefix}/*")
foreach(p ${stray})
    if(p MATCHES "(^|/)(\\.git|\\.github|\\.gitignore|bricks)$" OR p MATCHES "^bin/clay-native\\.cfg$")
        message(FATAL_ERROR "should not be installed: ${p}")
    endif()
    if(IS_SYMLINK "${prefix}/${p}")     # would dangle once archived and unpacked
        message(FATAL_ERROR "installed as a symlink: ${p}")
    endif()
endforeach()

function(run_clay prog want what)
    execute_process(COMMAND "${clay}" ${prog} WORKING_DIRECTORY "${WORK}"
                    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    string(REPLACE "\r" "" out "${out}")     # Windows line ends
    if(NOT rc EQUAL 0 OR NOT out MATCHES "${want}")
        message(FATAL_ERROR "${what}: installed clay failed (exit ${rc}), wanted '${want}':\n${out}\n${err}")
    endif()
endfunction()

# 3. the installed clay runs and knows its version
run_clay("--version" "clay ${VERSION}" "--version")

# 4. it imports the bundled bricks, and they are the installed copies: a marker
#    added to the installed stdlib/json shows up (not the checkout's lib/)
file(APPEND "${prefix}/lib/clay/stdlib/json/src/main.clay" "\n\"installed json\" println\n")
file(WRITE "${WORK}/prog.clay"
"import(\"stdlib/json\")
import(\"web/router\")
(Json stringify([1, 2])) println
(Router type) println
")
run_clay("${WORK}/prog.clay" "installed json\n\\[1, ?2\\]\nRouter" "bundled bricks")

# 5. the native toolchain files are relative to the install, not to this build
set(native "${prefix}/lib/clay/native")
foreach(f clay-native.cfg include/clay.h include/range.h include/hash.h include/clay_plugin.h)
    if(NOT EXISTS "${native}/${f}")
        message(FATAL_ERROR "not installed: lib/clay/native/${f}")
    endif()
endforeach()
file(READ "${native}/clay-native.cfg" cfg)
foreach(dir "${BUILD}" "${SOURCE}")
    string(FIND "${cfg}" "${dir}" at)
    if(NOT at EQUAL -1)
        message(FATAL_ERROR "installed clay-native.cfg names ${dir}:\n${cfg}")
    endif()
endforeach()

function(run_in dir want what)
    execute_process(COMMAND "${clay}" ${ARGN} WORKING_DIRECTORY "${dir}"
                    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    string(REPLACE "\r" "" out "${out}")
    if(NOT rc EQUAL 0 OR NOT out MATCHES "${want}")
        message(FATAL_ERROR "${what}: installed clay failed (exit ${rc}), wanted '${want}':\n${out}\n${err}")
    endif()
endfunction()
function(run_exe exe want what)
    execute_process(COMMAND "${exe}" RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    string(REPLACE "\r" "" out "${out}")
    if(NOT rc EQUAL 0 OR NOT out MATCHES "${want}")
        message(FATAL_ERROR "${what} failed (exit ${rc}), wanted '${want}':\n${out}\n${err}")
    endif()
endfunction()
if(CMAKE_HOST_WIN32)
    set(exe_suffix .exe)
endif()

# 6. --compile a program that uses a bundled brick, and run it
run_in("${WORK}" "built" "--compile" --compile -o "${WORK}/prog_native${exe_suffix}" "${WORK}/prog.clay")
run_exe("${WORK}/prog_native${exe_suffix}" "installed json\n\\[1, ?2\\]\nRouter" "the compiled program")

# 7. a native brick: its plugin built on first use (VM), and linked by --compile
run_in("${WORK}" "" "bricks new --native" bricks new nat-kit --native)
run_in("${WORK}/nat-kit" "nat-kit tests passed" "a native brick on the VM" tests/test_nat_kit.clay)
run_in("${WORK}/nat-kit" "built" "--compile of a native brick"
       --compile -o "${WORK}/nat_kit_native${exe_suffix}" tests/test_nat_kit.clay)
run_exe("${WORK}/nat_kit_native${exe_suffix}" "nat-kit tests passed" "the compiled native brick")

message(STATUS "install_test: ok (${prefix})")
