# expect_compare(rc output expected_file) — shared by expect_output.cmake and
# native_test.cmake: fail unless rc is 0 and output matches the golden file.
function(expect_compare rc output expected_file)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "program exited with ${rc}:\n${output}")
    endif()
    file(READ "${expected_file}" want)
    string(REPLACE "\r\n" "\n" got "${output}")
    string(REPLACE "\r\n" "\n" want "${want}")
    if(NOT got STREQUAL want)
        message(FATAL_ERROR "output differs from ${expected_file}\n"
                            "--- expected ---\n${want}--- got ---\n${got}")
    endif()
endfunction()
