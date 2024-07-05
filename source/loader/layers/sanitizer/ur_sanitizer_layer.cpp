/*
 *
 * Copyright (C) 2023 Intel Corporation
 *
 * Part of the Unified-Runtime Project, under the Apache License v2.0 with LLVM Exceptions.
 * See LICENSE.TXT
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 * @file ur_sanitizer_layer.cpp
 *
 */

#include "ur_sanitizer_layer.hpp"
#include "asan_interceptor.hpp"

namespace ur_sanitizer_layer {
context_t *getContext() { return context_t::get(); }

///////////////////////////////////////////////////////////////////////////////
context_t::context_t()
    : logger(logger::create_logger("sanitizer", false, false,
                                   logger::Level::WARN)),
      interceptor(std::make_unique<SanitizerInterceptor>()) {}

bool context_t::isAvailable() const { return true; }

ur_result_t context_t::tearDown() { return UR_RESULT_SUCCESS; }

///////////////////////////////////////////////////////////////////////////////
context_t::~context_t() {}
} // namespace ur_sanitizer_layer
