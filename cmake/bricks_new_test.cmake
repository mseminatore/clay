# bricks_new_test.cmake — `clay bricks new` makes a brick whose own tests pass:
# a pure Clay brick and a native one (its C plugin is built on first use), run
# with the VM and, for the native one, also as a compiled executable.
#
#   cmake -DCLAY=<clay binary> -DWORK=<scratch dir> -P bricks_new_test.cmake

foreach(var CLAY WORK)
    if(NOT ${var})
        message(FATAL_ERROR "bricks_new_test.cmake: ${var} is not set")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")

function(run_ok what dir)
    execute_process(COMMAND ${ARGN} WORKING_DIRECTORY "${dir}" TIMEOUT 300
                    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "${what} failed (${rc}):\n${out}")
    endif()
    set(last_output "${out}" PARENT_SCOPE)
endfunction()

# a pure Clay brick
run_ok("bricks new" "${WORK}" "${CLAY}" bricks new demo-kit)
foreach(f brick.json src/main.clay tests/test_demo_kit.clay README.md .gitignore)
    if(NOT EXISTS "${WORK}/demo-kit/${f}")
        message(FATAL_ERROR "bricks new did not create demo-kit/${f}")
    endif()
endforeach()
run_ok("the tests of demo-kit" "${WORK}/demo-kit" "${CLAY}" tests/test_demo_kit.clay)
if(NOT last_output MATCHES "demo-kit tests passed")
    message(FATAL_ERROR "demo-kit tests did not report success:\n${last_output}")
endif()

# a native brick, imported by its own name: the VM builds the plugin on first use
run_ok("bricks new --native" "${WORK}" "${CLAY}" bricks new nat-kit --native)
foreach(f brick.json src/main.clay native/nat_kit.c tests/test_nat_kit.clay README.md .gitignore)
    if(NOT EXISTS "${WORK}/nat-kit/${f}")
        message(FATAL_ERROR "bricks new --native did not create nat-kit/${f}")
    endif()
endforeach()
run_ok("the tests of nat-kit" "${WORK}/nat-kit" "${CLAY}" tests/test_nat_kit.clay)
if(NOT last_output MATCHES "nat-kit tests passed")
    message(FATAL_ERROR "nat-kit tests did not report success:\n${last_output}")
endif()

# ...and compiled: the executable links the brick's C in directly
set(exe "${WORK}/nat-kit/tests/nat_kit_test${CMAKE_EXECUTABLE_SUFFIX}")
run_ok("clay --compile of the nat-kit tests" "${WORK}/nat-kit"
       "${CLAY}" --compile -o "${exe}" tests/test_nat_kit.clay)
run_ok("the compiled nat-kit tests" "${WORK}/nat-kit" "${exe}")
if(NOT last_output MATCHES "nat-kit tests passed")
    message(FATAL_ERROR "compiled nat-kit tests did not report success:\n${last_output}")
endif()

# mistakes are refused
execute_process(COMMAND "${CLAY}" bricks new Bad_Name WORKING_DIRECTORY "${WORK}"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
if(rc EQUAL 0 OR NOT out MATCHES "invalid brick name")
    message(FATAL_ERROR "a bad name should be refused:\n${out}")
endif()
execute_process(COMMAND "${CLAY}" bricks new demo-kit WORKING_DIRECTORY "${WORK}"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
if(rc EQUAL 0 OR NOT out MATCHES "already exists")
    message(FATAL_ERROR "an existing folder should be refused:\n${out}")
endif()
message(STATUS "bricks new makes bricks whose tests pass")
