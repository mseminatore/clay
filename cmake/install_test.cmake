# install_test.cmake -- `cmake --install` produces a Clay that works on its own:
# bin/clay plus lib/clay/{stdlib,web}, the bundled bricks found next to the
# executable (CLAY_LIB unset), and no git metadata or build-machine files.
#
#   cmake -DBUILD=<build dir> [-DCONFIG=<config>] -DVERSION=<x.y.z> -DWORK=<scratch dir>
#         -P install_test.cmake

foreach(var BUILD VERSION WORK)
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
    if(p MATCHES "(^|/)(\\.git|\\.github|\\.gitignore|bricks|clay-native\\.cfg)$")
        message(FATAL_ERROR "should not be installed: ${p}")
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

message(STATUS "install_test: ok (${prefix})")
