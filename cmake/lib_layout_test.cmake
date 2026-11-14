# lib_layout_test.cmake -- an unpacked or installed Clay finds its bundled bricks
# (lib/) next to the executable, with no source checkout around and CLAY_LIB unset.
# Each layout gets a copy of clay and a fake stdlib brick that prints a marker, so
# the test proves the lib/ beside the copy was used and not the checkout's.
#
#   cmake -DCLAY=<clay binary> -DROOT=<source dir, to check selfhost too> -DWORK=<scratch dir>
#         -P lib_layout_test.cmake

foreach(var CLAY WORK)
    if(NOT ${var})
        message(FATAL_ERROR "lib_layout_test.cmake: ${var} is not set")
    endif()
endforeach()
unset(ENV{CLAY_LIB})

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
file(WRITE "${WORK}/prog.clay" "import(\"stdlib\")\n")

# a layout: where the executable goes, and where lib/ goes relative to the work dir
function(make_layout name bin_dir lib_dir)
    set(root "${WORK}/${name}")
    file(MAKE_DIRECTORY "${root}/${bin_dir}" "${root}/${lib_dir}/stdlib/src")
    file(WRITE "${root}/${lib_dir}/stdlib/brick.json"
         "{\"name\": \"stdlib\", \"version\": \"0.0.0\", \"main\": \"src/main.clay\"}\n")
    file(WRITE "${root}/${lib_dir}/stdlib/src/main.clay" "\"lib beside ${name}\" println\n")
endfunction()

function(check_runs exe name)
    execute_process(COMMAND "${exe}" "${WORK}/prog.clay"
                    WORKING_DIRECTORY "${WORK}"
                    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL 0 OR NOT out MATCHES "lib beside ${name}")
        message(FATAL_ERROR "${name}: ${exe} did not use the lib/ beside it (exit ${rc}):\n${out}\n${err}")
    endif()
endfunction()

# 1. a release archive: clay and lib/ side by side
make_layout(archive "." "lib")
file(COPY "${CLAY}" DESTINATION "${WORK}/archive")
get_filename_component(clay_name "${CLAY}" NAME)
check_runs("${WORK}/archive/${clay_name}" archive)

# 2. a prefix: bin/clay and lib/clay/
make_layout(prefix "bin" "lib/clay")
file(COPY "${CLAY}" DESTINATION "${WORK}/prefix/bin")
check_runs("${WORK}/prefix/bin/${clay_name}" prefix)

# 3. CLAY_LIB still wins over a lib/ beside the executable
make_layout(override "." "lib")
file(COPY "${CLAY}" DESTINATION "${WORK}/override")
make_layout(elsewhere "." "lib")
set(ENV{CLAY_LIB} "${WORK}/elsewhere/lib")
execute_process(COMMAND "${WORK}/override/${clay_name}" "${WORK}/prog.clay"
                WORKING_DIRECTORY "${WORK}"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
unset(ENV{CLAY_LIB})
if(NOT rc EQUAL 0 OR NOT out MATCHES "lib beside elsewhere")
    message(FATAL_ERROR "CLAY_LIB did not take precedence (exit ${rc}):\n${out}\n${err}")
endif()

# 4. the selfhost parser's copy of the lookup (Parser libDir), run by the copied clay
if(ROOT)
    file(WRITE "${WORK}/libdir.clay"
         "import(\"${ROOT}/selfhost/clay-parser/src/main.clay\")\n(Parser libDir) println\n")
    foreach(layout archive prefix)
        if(layout STREQUAL "archive")
            set(exe "${WORK}/archive/${clay_name}")
            set(want "${WORK}/archive/lib")
        else()
            set(exe "${WORK}/prefix/bin/${clay_name}")
            set(want "${WORK}/prefix/bin/../lib/clay")
        endif()
        execute_process(COMMAND "${exe}" "${WORK}/libdir.clay" WORKING_DIRECTORY "${WORK}"
                        RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
        string(STRIP "${out}" out)
        file(TO_CMAKE_PATH "${out}" out)     # Windows: the exe path has backslashes
        if(NOT rc EQUAL 0 OR NOT out STREQUAL "${want}")
            message(FATAL_ERROR "selfhost Parser libDir in ${layout}: want ${want}, got '${out}' (${rc}):\n${err}")
        endif()
    endforeach()
endif()

message(STATUS "lib_layout_test: ok")
