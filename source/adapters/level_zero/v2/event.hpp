//===--------- event.hpp - Level Zero Adapter -----------------------------===//
//
// Copyright (C) 2024 Intel Corporation
//
// Part of the Unified-Runtime Project, under the Apache License v2.0 with LLVM
// Exceptions. See LICENSE.TXT
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#pragma once

#include <stack>

#include "../common.hpp"
#include <ur/ur.hpp>
#include <ur_api.h>
#include <ze_api.h>

#include "event_provider.hpp"

namespace v2 {

class event_pool;

class ur_event_handle_t_;
using ur_event_handle_t = ur_event_handle_t_ *;

class ur_event_handle_t_ : _ur_object {
public:
  ur_event_handle_t_(event_allocation eventAllocation, event_pool *pool);

  void reset();
  ze_event_handle_t getZeEvent() const;

  ur_result_t retain();
  ur_result_t release();

private:
  event_type type;
  raii::cache_borrowed_event zeEvent;
  event_pool *pool;
};

} // namespace v2
