# Top-level ESP-IDF project file for Brimbelle.
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(brimbelle)

# Inherited from Bugne: at -O2, GCC raises two false positives in the vendored
# sendspin-cpp (maybe-uninitialized on a std::optional, dangling-reference on an
# ArduinoJson view). Keep the warnings but do not fail the build on them.
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=maybe-uninitialized" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=dangling-reference" APPEND)

# Patch the vendored sendspin-cpp after the component manager fetches it: the
# httpd task stack goes from 4 KB to 8 KB (the default corrupts the LWIP select
# semaphore under load). managed_components/ is not tracked, so this re-applies
# on every configure. Idempotent. See patches/ and UPSTREAM.md.
set(_ss_dir "${CMAKE_CURRENT_LIST_DIR}/managed_components/sendspin__sendspin-cpp")
set(_ss_ws "${_ss_dir}/src/esp/ws_server.cpp")
set(_ss_patch "${CMAKE_CURRENT_LIST_DIR}/patches/sendspin-cpp-0.8.0-esp-httpd-stack.patch")
if(EXISTS "${_ss_ws}")
    file(READ "${_ss_ws}" _ss_ws_src)
    string(FIND "${_ss_ws_src}" "config.stack_size = 8192" _ss_marker)
    if(_ss_marker EQUAL -1)
        execute_process(
            COMMAND patch -p1 -i "${_ss_patch}"
            WORKING_DIRECTORY "${_ss_dir}"
            RESULT_VARIABLE _ss_rc OUTPUT_VARIABLE _ss_out ERROR_VARIABLE _ss_out)
        if(NOT _ss_rc EQUAL 0)
            message(FATAL_ERROR "Failed to apply sendspin-cpp patch:\n${_ss_out}")
        endif()
        message(STATUS "Applied sendspin-cpp httpd stack patch")
    endif()
endif()
