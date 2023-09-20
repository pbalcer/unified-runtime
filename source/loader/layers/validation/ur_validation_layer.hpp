/*
 *
 * Copyright (C) 2023 Corporation
 *
 * Part of the Unified-Runtime Project, under the Apache License v2.0 with LLVM Exceptions.
 * See LICENSE.TXT
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 * @file ur_layer.h
 *
 */
#pragma once
#include "logger/ur_logger.hpp"
#include "ur_ddi.h"
#include "ur_proxy_layer.hpp"
#include "ur_util.hpp"

namespace ur_validation_layer {

///////////////////////////////////////////////////////////////////////////////
class __urdlllocal context_t : public proxy_layer_context_t {
  public:
    bool enableParameterValidation = false;
    bool enableLeakChecking = false;
    logger::Logger logger;

    ur_dditable_t urDdiTable = {};

    context_t();
    ~context_t();

    bool isAvailable() const override { return true; }
    std::vector<std::string> getNames() const override {
        return {nameFullValidation, nameParameterValidation, nameLeakChecking};
    }
    ur_result_t init(ur_dditable_t *dditable,
                     const std::set<std::string> &enabledLayerNames) override;
    ur_result_t tearDown() override;

  private:
    const std::string nameFullValidation = "UR_LAYER_FULL_VALIDATION";
    const std::string nameParameterValidation = "UR_LAYER_PARAMETER_VALIDATION";
    const std::string nameLeakChecking = "UR_LAYER_LEAK_CHECKING";
};

extern context_t context;

} // namespace ur_validation_layer
