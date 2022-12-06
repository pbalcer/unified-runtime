# Copyright (C) 2022 Intel Corporation
# SPDX-License-Identifier: MIT

add_test(NAME python-basic
        WORKING_DIRECTORY ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}
        COMMAND ${Python3_EXECUTABLE} -m pytest ${CMAKE_CURRENT_SOURCE_DIR}/basic.py
)
