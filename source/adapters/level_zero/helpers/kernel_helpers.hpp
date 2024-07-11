//===--------- kernel.cpp - Level Zero Adapter ----------------------------===//
//
// Copyright (C) 2023 Intel Corporation
//
// Part of the Unified-Runtime Project, under the Apache License v2.0 with LLVM
// Exceptions. See LICENSE.TXT
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include <ur_api.h>
#include <ze_api.h>

#include "../common.hpp"
#include "logger/ur_logger.hpp"

/**
 * Calculates a work group size for the kernel based on the GlobalWorkSize or
 * the LocalWorkSize if provided.
 * @param[in][optional] Kernel The Kernel. Used when LocalWorkSize is not
 * provided.
 * @param[in][optional] Device The device associated with the kernel. Used when
 * LocalWorkSize is not provided.
 * @param[out] ZeThreadGroupDimensions Number of work groups in each dimension.
 * @param[out] WG The work group size for each dimension.
 * @param[in] WorkDim The number of dimensions in the kernel.
 * @param[in] GlobalWorkSize The global work size.
 * @param[in][optional] LocalWorkSize The local work size.
 * @return UR_RESULT_SUCCESS or an error code on failure.
 */
ur_result_t calculateKernelWorkDimensions(
    ze_kernel_handle_t Kernel, ur_device_handle_t Device,
    ze_group_count_t &ZeThreadGroupDimensions, uint32_t (&WG)[3],
    uint32_t WorkDim, const size_t *GlobalWorkSize,
    const size_t *LocalWorkSize);

/**
 * Sets the global offset for a kernel command that will be appended to the
 * command buffer.
 * @param[in] Context Context associated with the queue.
 * @param[in] Kernel The handle to the kernel that will be appended.
 * @param[in] GlobalWorkOffset The global offset value.
 * @return UR_RESULT_SUCCESS or an error code on failure
 */
template <typename ContextHandle>
ur_result_t setKernelGlobalOffset(ContextHandle Context,
                                  ze_kernel_handle_t Kernel,
                                  const size_t *GlobalWorkOffset) {

  if (!Context->getPlatform()->ZeDriverGlobalOffsetExtensionFound) {
    logger::debug("No global offset extension found on this driver");
    return UR_RESULT_ERROR_INVALID_VALUE;
  }

  ZE2UR_CALL(
      zeKernelSetGlobalOffsetExp,
      (Kernel, GlobalWorkOffset[0], GlobalWorkOffset[1], GlobalWorkOffset[2]));

  return UR_RESULT_SUCCESS;
}

ur_result_t getSuggestedLocalWorkSize(ur_device_handle_t hDevice,
                                      ze_kernel_handle_t hZeKernel,
                                      size_t GlobalWorkSize3D[3],
                                      uint32_t SuggestedLocalWorkSize3D[3]);
