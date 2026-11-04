# regen_vm.cmake — rebuild vmgen from deps/vmgen and regenerate src/vm/generated/
#
# Usage (from the repo root, any platform):
#   cmake -P cmake/regen_vm.cmake           # regenerate in place
#   cmake -P cmake/regen_vm.cmake -- --check  # fail if generated files are stale
#
# Or via the build: cmake --build build --target regen_vm
#
# Requires the vmgen submodule:  git submodule update --init --recursive

cmake_minimum_required(VERSION 3.10)

get_filename_component(CLAY_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(VMGEN_SRC   "${CLAY_ROOT}/deps/vmgen")
set(VMGEN_BUILD "${CLAY_ROOT}/build/_vmgen")
set(VM_SPEC     "${CLAY_ROOT}/src/vm/clay.yaml")
set(VM_OUT      "${CLAY_ROOT}/src/vm/generated")
set(VM_TMP      "${CLAY_ROOT}/build/_vmgen_out")

set(CHECK_ONLY OFF)
foreach(i RANGE ${CMAKE_ARGC})
    if("${CMAKE_ARGV${i}}" STREQUAL "--check")
        set(CHECK_ONLY ON)
    endif()
endforeach()

if(NOT EXISTS "${VMGEN_SRC}/CMakeLists.txt")
    message(FATAL_ERROR "deps/vmgen is missing. Run: git submodule update --init --recursive")
endif()

# 1. Build vmgen (Release, out of tree)
execute_process(
    COMMAND ${CMAKE_COMMAND} -S "${VMGEN_SRC}" -B "${VMGEN_BUILD}" -DCMAKE_BUILD_TYPE=Release
    RESULT_VARIABLE rc OUTPUT_QUIET)
if(rc)
    message(FATAL_ERROR "configuring vmgen failed")
endif()
execute_process(
    COMMAND ${CMAKE_COMMAND} --build "${VMGEN_BUILD}" --config Release --target vmgen
    RESULT_VARIABLE rc OUTPUT_QUIET)
if(rc)
    message(FATAL_ERROR "building vmgen failed")
endif()

find_program(VMGEN_EXE vmgen
    PATHS "${VMGEN_BUILD}" "${VMGEN_BUILD}/Release" "${VMGEN_BUILD}/Debug"
    NO_DEFAULT_PATH)
if(NOT VMGEN_EXE)
    message(FATAL_ERROR "vmgen executable not found under ${VMGEN_BUILD}")
endif()

# 2. Generate into a scratch directory
file(REMOVE_RECURSE "${VM_TMP}")
execute_process(
    COMMAND "${VMGEN_EXE}" "${VM_SPEC}" -o "${VM_TMP}"
    RESULT_VARIABLE rc OUTPUT_QUIET)
if(rc)
    message(FATAL_ERROR "vmgen failed on ${VM_SPEC}")
endif()

# 3. Compare (check mode) or copy into src/vm/generated
file(GLOB generated RELATIVE "${VM_TMP}" "${VM_TMP}/*")
set(stale "")
foreach(f ${generated})
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E compare_files "${VM_TMP}/${f}" "${VM_OUT}/${f}"
        RESULT_VARIABLE differs OUTPUT_QUIET ERROR_QUIET)
    if(differs)
        list(APPEND stale "${f}")
        if(NOT CHECK_ONLY)
            file(COPY "${VM_TMP}/${f}" DESTINATION "${VM_OUT}")
        endif()
    endif()
endforeach()

if(CHECK_ONLY)
    if(stale)
        message(FATAL_ERROR "src/vm/generated is out of date with clay.yaml: ${stale}\n"
                            "Run: cmake -P cmake/regen_vm.cmake")
    endif()
    message(STATUS "src/vm/generated is up to date")
elseif(stale)
    message(STATUS "regenerated: ${stale}")
else()
    message(STATUS "src/vm/generated already up to date")
endif()
