# Copyright (C) 2023 Intel Corporation
# SPDX-License-Identifier: MIT

#
# helpers.cmake -- helper functions for top-level CMakeLists.txt
#

if(NOT DEFINED TEST_FILE)
    message(FATAL_ERROR "TEST_FILE needs to be defined")
endif()
if(NOT DEFINED MATCH_FILE)
    message(FATAL_ERROR "MATCH_FILE needs to be defined")
endif()

set(TEST_OUT "_matchtmpfile")

execute_process(
    COMMAND ${TEST_FILE}
    OUTPUT_FILE ${TEST_OUT}
    RESULT_VARIABLE TEST_RESULT
)

if(TEST_RESULT)
    message(FATAL_ERROR "FAILED: Test ${TEST_FILE} returned non-zero (${TEST_ERROR}).")
endif()

execute_process(
    COMMAND ${CMAKE_COMMAND} -E compare_files ${TEST_OUT} ${MATCH_FILE}
    RESULT_VARIABLE TEST_RESULT
)

if(TEST_RESULT)
    message(FATAL_ERROR "FAILED: The output of ${TEST_FILE} (stored in ${TEST_OUT}) does not match ${MATCH_FILE}")
elseif()
    message("Passed: The output of ${TEST_FILE} matches ${MATCH_FILE}")
endif()
