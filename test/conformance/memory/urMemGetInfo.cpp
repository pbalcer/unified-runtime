// Copyright (C) 2023 Intel Corporation
// Part of the Unified-Runtime Project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.TXT
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#include <map>
#include <uur/fixtures.h>

using urMemGetInfoTest = uur::urMemBufferTestWithParam<ur_mem_info_t>;

static std::unordered_map<ur_mem_info_t, size_t> mem_info_size_map = {
    {UR_MEM_INFO_SIZE, sizeof(size_t)},
    {UR_MEM_INFO_CONTEXT, sizeof(ur_context_handle_t)},
};

UUR_TEST_SUITE_P(urMemGetInfoTest,
                 ::testing::Values(UR_MEM_INFO_SIZE, UR_MEM_INFO_CONTEXT),
                 uur::deviceTestWithParamPrinter<ur_mem_info_t>);

TEST_P(urMemGetInfoTest, Success) {
    ur_mem_info_t info = getParam();
    size_t size;
    ASSERT_SUCCESS(urMemGetInfo(buffer, info, 0, nullptr, &size));
    ASSERT_NE(size, 0);

    if (const auto expected_size = mem_info_size_map.find(info);
        expected_size != mem_info_size_map.end()) {
        ASSERT_EQ(expected_size->second, size);
    }

    std::vector<uint8_t> info_data(size);
    ASSERT_SUCCESS(urMemGetInfo(buffer, info, size, info_data.data(), nullptr));
}

TEST_P(urMemGetInfoTest, InvalidNullHandleMemory) {
    size_t mem_size = 0;
    ASSERT_EQ_RESULT(UR_RESULT_ERROR_INVALID_NULL_HANDLE,
                     urMemGetInfo(nullptr, UR_MEM_INFO_SIZE, sizeof(size_t),
                                  &mem_size, nullptr));
}

TEST_P(urMemGetInfoTest, InvalidEnumerationMemInfoType) {
    size_t mem_size = 0;
    ASSERT_EQ_RESULT(UR_RESULT_ERROR_INVALID_ENUMERATION,
                     urMemGetInfo(buffer, UR_MEM_INFO_FORCE_UINT32,
                                  sizeof(size_t), &mem_size, nullptr));
}

TEST_P(urMemGetInfoTest, InvalidSizeZero) {
    size_t mem_size = 0;
    ASSERT_EQ_RESULT(
        urMemGetInfo(buffer, UR_MEM_INFO_SIZE, 0, &mem_size, nullptr),
        UR_RESULT_ERROR_INVALID_SIZE);
}

TEST_P(urMemGetInfoTest, InvalidSizeSmall) {
    size_t mem_size = 0;
    ASSERT_EQ_RESULT(urMemGetInfo(buffer, UR_MEM_INFO_SIZE,
                                  sizeof(mem_size) - 1, &mem_size, nullptr),
                     UR_RESULT_ERROR_INVALID_SIZE);
}

TEST_P(urMemGetInfoTest, InvalidNullPointerParamValue) {
    size_t mem_size = 0;
    ASSERT_EQ_RESULT(urMemGetInfo(buffer, UR_MEM_INFO_SIZE, sizeof(mem_size),
                                  nullptr, nullptr),
                     UR_RESULT_ERROR_INVALID_NULL_POINTER);
}

TEST_P(urMemGetInfoTest, InvalidNullPointerPropSizeRet) {
    ASSERT_EQ_RESULT(
        urMemGetInfo(buffer, UR_MEM_INFO_SIZE, 0, nullptr, nullptr),
        UR_RESULT_ERROR_INVALID_NULL_POINTER);
}
