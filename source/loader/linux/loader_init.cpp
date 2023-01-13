/*
 *
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "ur_loader.h"

namespace loader
{

    void __attribute__((constructor)) createLoaderContext() {
        context = new context_t;
    }

    void __attribute__((destructor)) deleteLoaderContext() {
        delete context;
    }

}
