# dap_test.cmake — drive `clay --dap` with a scripted session and check what
# it answers: a breakpoint in an imported file stops there (not at the same
# line of the main file), and the stack frames name that file and the code
# they are in.
#
#   cmake -DCLAY=<clay binary> -DROOT=<source dir> -DWORK=<scratch dir> -P dap_test.cmake
#
# The requests are all written up front; the adapter reads the ones after
# configurationDone when it stops at the breakpoint.

foreach(var CLAY ROOT WORK)
    if(NOT ${var})
        message(FATAL_ERROR "dap_test.cmake: ${var} is not set")
    endif()
endforeach()

set(dir "${ROOT}/test/dap")
file(MAKE_DIRECTORY "${WORK}")
set(input "")
set(seq 0)
macro(request json)
    math(EXPR seq "${seq} + 1")
    string(REPLACE "@SEQ@" "${seq}" body "${json}")
    string(LENGTH "${body}" len)
    string(APPEND input "Content-Length: ${len}\r\n\r\n${body}")
endmacro()
request([=[{"seq":@SEQ@,"type":"request","command":"initialize","arguments":{"adapterID":"clay"}}]=])
request("{\"seq\":@SEQ@,\"type\":\"request\",\"command\":\"launch\",\"arguments\":{\"program\":\"${dir}/main.clay\"}}")
request("{\"seq\":@SEQ@,\"type\":\"request\",\"command\":\"setBreakpoints\",\"arguments\":{\"source\":{\"path\":\"${dir}/lib.clay\"},\"breakpoints\":[{\"line\":4}]}}")
request([=[{"seq":@SEQ@,"type":"request","command":"configurationDone"}]=])
request([=[{"seq":@SEQ@,"type":"request","command":"stackTrace","arguments":{"threadId":1}}]=])
request([=[{"seq":@SEQ@,"type":"request","command":"continue","arguments":{"threadId":1}}]=])
file(WRITE "${WORK}/dap_requests.txt" "${input}")

execute_process(COMMAND "${CLAY}" --dap main.clay
                WORKING_DIRECTORY "${dir}"
                INPUT_FILE "${WORK}/dap_requests.txt"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
                TIMEOUT 30)

string(REGEX MATCHALL "\"event\":\"stopped\"" stops "${out}")
list(LENGTH stops nstops)
if(NOT nstops EQUAL 1)
    message(FATAL_ERROR "expected one stop, got ${nstops}:\n${out}\n${err}")
endif()
if(NOT out MATCHES "\"reason\":\"breakpoint\"")
    message(FATAL_ERROR "the stop was not at the breakpoint:\n${out}")
endif()
# frame 0: the imported file, line 4, with an absolute path
if(NOT out MATCHES "\"stackFrames\":\\[{\"id\":0,\"name\":\"[^\"]*\",\"source\":{\"name\":\"[^\"]*lib\\.clay\",\"path\":\"[^\"]*lib\\.clay\"},\"line\":4")
    message(FATAL_ERROR "frame 0 is not lib.clay line 4:\n${out}")
endif()
# frames are named by the code they are in, as tracebacks say it
if(NOT out MATCHES "{\"id\":0,\"name\":\"twice\"")
    message(FATAL_ERROR "frame 0 is not named after its method (twice):\n${out}")
endif()
if(NOT out MATCHES "\"name\":\"<top level>\",\"source\":{\"name\":\"main\\.clay\"")
    message(FATAL_ERROR "frame 1 is not main.clay at top level:\n${out}")
endif()
# the program ran to the end after continuing
if(NOT out MATCHES "42")
    message(FATAL_ERROR "the program did not finish after continue:\n${out}\n${err}")
endif()
