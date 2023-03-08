/*
 *
 * Copyright (C) 2023 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "uma/base.h"
#ifndef UMA_HELPERS_H
#define UMA_HELPERS_H 1

#include <uma/memory_pool.h>
#include <uma/memory_pool_ops.h>
#include <uma/memory_provider.h>
#include <uma/memory_provider_ops.h>

#include <memory>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace uma {
struct pool {
    virtual void *malloc(size_t) = 0;

    operator uma_memory_pool_handle_t() {
        uma_memory_pool_handle_t handle;
        uma_memory_pool_ops_t ops;
        ops.initialize = [](void *params, void **pool) {
            *pool = params;
            return UMA_RESULT_SUCCESS;
        };
        ops.finalize = [](void *pool) {
        };
        ops.version = UMA_VERSION_CURRENT;
        ops.malloc = [](void *ptr, size_t s) {
            struct pool *pool = static_cast<struct pool *>(ptr);
            return pool->malloc(s);
        };
        umaPoolCreate(&ops, this, &handle);

        return handle;
    };
};

struct malloc_pool : pool {
    void *malloc(size_t s) {
        return ::malloc(s);
    }
};

} // namespace uma

#endif /* UMA_HELPERS_H */