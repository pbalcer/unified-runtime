//===--------- command_buffer.cpp - Level Zero Adapter --------------------===//
//
// Copyright (C) 2023 Intel Corporation
//
// Part of the Unified-Runtime Project, under the Apache License v2.0 with LLVM
// Exceptions.
// See LICENSE.TXT SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#include "command_buffer.hpp"
#include "helpers/kernel_helpers.hpp"
#include "logger/ur_logger.hpp"
#include "ur_level_zero.hpp"

/* L0 Command-buffer Extension Doc see:
https://github.com/intel/llvm/blob/sycl/sycl/doc/design/CommandGraph.md#level-zero
*/

// Print the name of a variable and its value in the L0 debug log
#define DEBUG_LOG(VAR) logger::debug(#VAR " {}", VAR);

namespace {

// Gets a C pointer from a vector. If the vector is empty returns nullptr
// instead. This is different from the behaviour of the data() member function
// of the vector class which might not return nullptr when the vector is empty.
template <typename T> T *getPointerFromVector(std::vector<T> &V) {
  return V.size() == 0 ? nullptr : V.data();
}

/**
 * Default to using compute engine for fill operation, but allow to override
 * this with an environment variable. Disable the copy engine if the pattern
 * size is larger than the maximum supported.
 * @param[in] CommandBuffer The CommandBuffer where the fill command will be
 * appended.
 * @param[in] PatternSize The pattern size for the fill command.
 * @param[out] PreferCopyEngine Whether copy engine usage should be enabled or
 * disabled for fill commands.
 * @return UR_RESULT_SUCCESS or an error code on failure
 */
ur_result_t
preferCopyEngineForFill(ur_exp_command_buffer_handle_t CommandBuffer,
                        size_t PatternSize, bool &PreferCopyEngine) {
  assert(PatternSize > 0);

  PreferCopyEngine = false;
  if (!CommandBuffer->useCopyEngine()) {
    return UR_RESULT_SUCCESS;
  }

  // If the copy engine is available, and it supports this pattern size, the
  // command should be enqueued in the copy command list, otherwise enqueue it
  // in the compute command list.
  PreferCopyEngine =
      PatternSize <=
      CommandBuffer->Device
          ->QueueGroup[ur_device_handle_t_::queue_group_info_t::MainCopy]
          .ZeProperties.maxMemoryFillPatternSize;

  if (!PreferCopyEngine) {
    // Pattern size must fit the compute queue capabilities.
    UR_ASSERT(
        PatternSize <=
            CommandBuffer->Device
                ->QueueGroup[ur_device_handle_t_::queue_group_info_t::Compute]
                .ZeProperties.maxMemoryFillPatternSize,
        UR_RESULT_ERROR_INVALID_VALUE);
  }

  const char *UrRet = std::getenv("UR_L0_USE_COPY_ENGINE_FOR_FILL");
  const char *PiRet =
      std::getenv("SYCL_PI_LEVEL_ZERO_USE_COPY_ENGINE_FOR_FILL");

  PreferCopyEngine =
      PreferCopyEngine &&
      (UrRet ? std::stoi(UrRet) : (PiRet ? std::stoi(PiRet) : 0));

  return UR_RESULT_SUCCESS;
}

/**
 * Helper function for finding the Level Zero events associated with the
 * commands in a command-buffer, each event is pointed to by a sync-point in the
 * wait list.
 * @param[in] CommandBuffer to lookup the L0 events from.
 * @param[in] NumSyncPointsInWaitList Length of \p SyncPointWaitList.
 * @param[in] SyncPointWaitList List of sync points in \p CommandBuffer to find
 * the L0 events for.
 * @param[out] ZeEventList Return parameter for the L0 events associated with
 * each sync-point in \p SyncPointWaitList.
 * @return UR_RESULT_SUCCESS or an error code on failure
 */
ur_result_t getEventsFromSyncPoints(
    const ur_exp_command_buffer_handle_t &CommandBuffer,
    size_t NumSyncPointsInWaitList,
    const ur_exp_command_buffer_sync_point_t *SyncPointWaitList,
    std::vector<ze_event_handle_t> &ZeEventList) {
  if (!SyncPointWaitList || NumSyncPointsInWaitList == 0)
    return UR_RESULT_SUCCESS;

  // For each sync-point add associated L0 event to the return list.
  for (size_t i = 0; i < NumSyncPointsInWaitList; i++) {
    if (auto EventHandle = CommandBuffer->SyncPoints.find(SyncPointWaitList[i]);
        EventHandle != CommandBuffer->SyncPoints.end()) {
      ZeEventList.push_back(EventHandle->second->ZeEvent);
    } else {
      return UR_RESULT_ERROR_INVALID_VALUE;
    }
  }
  return UR_RESULT_SUCCESS;
}

/**
 * If needed, creates a sync point for a given command and returns the L0
 * events associated with the sync point.
 * This operations is skipped if the command buffer is in order.
 * @param[in] CommandType The type of the command.
 * @param[in] CommandBuffer The CommandBuffer where the command is appended.
 * @param[in] NumSyncPointsInWaitList Number of sync points that are
 * dependencies for the command.
 * @param[in] SyncPointWaitList List of sync point that are dependencies for the
 * command.
 * @param[in] HostVisible Whether the event associated with the sync point
 * should be host visible.
 * @param[out][optional] RetSyncPoint The new sync point.
 * @param[out] ZeEventList A list of L0 events that are dependencies for this
 * sync point.
 * @param[out] ZeLaunchEvent The L0 event associated with this sync point.
 * @return UR_RESULT_SUCCESS or an error code on failure
 */
ur_result_t createSyncPointAndGetZeEvents(
    ur_command_t CommandType, ur_exp_command_buffer_handle_t CommandBuffer,
    uint32_t NumSyncPointsInWaitList,
    const ur_exp_command_buffer_sync_point_t *SyncPointWaitList,
    bool HostVisible, ur_exp_command_buffer_sync_point_t *RetSyncPoint,
    std::vector<ze_event_handle_t> &ZeEventList,
    ze_event_handle_t &ZeLaunchEvent) {

  ZeLaunchEvent = nullptr;

  if (CommandBuffer->IsInOrderCmdList) {
    return UR_RESULT_SUCCESS;
  }

  UR_CALL(getEventsFromSyncPoints(CommandBuffer, NumSyncPointsInWaitList,
                                  SyncPointWaitList, ZeEventList));
  ur_event_handle_t LaunchEvent;
  UR_CALL(EventCreate(CommandBuffer->Context, nullptr, false, HostVisible,
                      &LaunchEvent, false, !CommandBuffer->IsProfilingEnabled));
  LaunchEvent->CommandType = CommandType;
  ZeLaunchEvent = LaunchEvent->ZeEvent;

  // Get sync point and register the event with it.
  ur_exp_command_buffer_sync_point_t SyncPoint =
      CommandBuffer->getNextSyncPoint();
  CommandBuffer->registerSyncPoint(SyncPoint, LaunchEvent);

  if (RetSyncPoint) {
    *RetSyncPoint = SyncPoint;
  }

  return UR_RESULT_SUCCESS;
}

// Shared by all memory read/write/copy PI interfaces.
// Helper function for common code when enqueuing memory operations to a command
// buffer.
ur_result_t enqueueCommandBufferMemCopyHelper(
    ur_command_t CommandType, ur_exp_command_buffer_handle_t CommandBuffer,
    void *Dst, const void *Src, size_t Size, bool PreferCopyEngine,
    uint32_t NumSyncPointsInWaitList,
    const ur_exp_command_buffer_sync_point_t *SyncPointWaitList,
    ur_exp_command_buffer_sync_point_t *RetSyncPoint) {

  std::vector<ze_event_handle_t> ZeEventList;
  ze_event_handle_t ZeLaunchEvent = nullptr;
  UR_CALL(createSyncPointAndGetZeEvents(
      CommandType, CommandBuffer, NumSyncPointsInWaitList, SyncPointWaitList,
      false, RetSyncPoint, ZeEventList, ZeLaunchEvent));

  ze_command_list_handle_t ZeCommandList =
      CommandBuffer->chooseCommandList(PreferCopyEngine);

  ZE2UR_CALL(zeCommandListAppendMemoryCopy,
             (ZeCommandList, Dst, Src, Size, ZeLaunchEvent, ZeEventList.size(),
              getPointerFromVector(ZeEventList)));

  return UR_RESULT_SUCCESS;
}

// Helper function for common code when enqueuing rectangular memory operations
// to a command buffer.
ur_result_t enqueueCommandBufferMemCopyRectHelper(
    ur_command_t CommandType, ur_exp_command_buffer_handle_t CommandBuffer,
    void *Dst, const void *Src, ur_rect_offset_t SrcOrigin,
    ur_rect_offset_t DstOrigin, ur_rect_region_t Region, size_t SrcRowPitch,
    size_t DstRowPitch, size_t SrcSlicePitch, size_t DstSlicePitch,
    bool PreferCopyEngine, uint32_t NumSyncPointsInWaitList,
    const ur_exp_command_buffer_sync_point_t *SyncPointWaitList,
    ur_exp_command_buffer_sync_point_t *RetSyncPoint) {

  uint32_t SrcOriginX = ur_cast<uint32_t>(SrcOrigin.x);
  uint32_t SrcOriginY = ur_cast<uint32_t>(SrcOrigin.y);
  uint32_t SrcOriginZ = ur_cast<uint32_t>(SrcOrigin.z);

  uint32_t SrcPitch = SrcRowPitch;
  if (SrcPitch == 0)
    SrcPitch = ur_cast<uint32_t>(Region.width);

  if (SrcSlicePitch == 0)
    SrcSlicePitch = ur_cast<uint32_t>(Region.height) * SrcPitch;

  uint32_t DstOriginX = ur_cast<uint32_t>(DstOrigin.x);
  uint32_t DstOriginY = ur_cast<uint32_t>(DstOrigin.y);
  uint32_t DstOriginZ = ur_cast<uint32_t>(DstOrigin.z);

  uint32_t DstPitch = DstRowPitch;
  if (DstPitch == 0)
    DstPitch = ur_cast<uint32_t>(Region.width);

  if (DstSlicePitch == 0)
    DstSlicePitch = ur_cast<uint32_t>(Region.height) * DstPitch;

  uint32_t Width = ur_cast<uint32_t>(Region.width);
  uint32_t Height = ur_cast<uint32_t>(Region.height);
  uint32_t Depth = ur_cast<uint32_t>(Region.depth);

  const ze_copy_region_t ZeSrcRegion = {SrcOriginX, SrcOriginY, SrcOriginZ,
                                        Width,      Height,     Depth};
  const ze_copy_region_t ZeDstRegion = {DstOriginX, DstOriginY, DstOriginZ,
                                        Width,      Height,     Depth};

  std::vector<ze_event_handle_t> ZeEventList;
  ze_event_handle_t ZeLaunchEvent = nullptr;
  UR_CALL(createSyncPointAndGetZeEvents(
      CommandType, CommandBuffer, NumSyncPointsInWaitList, SyncPointWaitList,
      false, RetSyncPoint, ZeEventList, ZeLaunchEvent));

  ze_command_list_handle_t ZeCommandList =
      CommandBuffer->chooseCommandList(PreferCopyEngine);

  ZE2UR_CALL(zeCommandListAppendMemoryCopyRegion,
             (ZeCommandList, Dst, &ZeDstRegion, DstPitch, DstSlicePitch, Src,
              &ZeSrcRegion, SrcPitch, SrcSlicePitch, ZeLaunchEvent,
              ZeEventList.size(), getPointerFromVector(ZeEventList)));

  return UR_RESULT_SUCCESS;
}

// Helper function for enqueuing memory fills.
ur_result_t enqueueCommandBufferFillHelper(
    ur_command_t CommandType, ur_exp_command_buffer_handle_t CommandBuffer,
    void *Ptr, const void *Pattern, size_t PatternSize, size_t Size,
    uint32_t NumSyncPointsInWaitList,
    const ur_exp_command_buffer_sync_point_t *SyncPointWaitList,
    ur_exp_command_buffer_sync_point_t *RetSyncPoint) {
  // Pattern size must be a power of two.
  UR_ASSERT((PatternSize > 0) && ((PatternSize & (PatternSize - 1)) == 0),
            UR_RESULT_ERROR_INVALID_VALUE);

  std::vector<ze_event_handle_t> ZeEventList;
  ze_event_handle_t ZeLaunchEvent = nullptr;
  UR_CALL(createSyncPointAndGetZeEvents(
      CommandType, CommandBuffer, NumSyncPointsInWaitList, SyncPointWaitList,
      true, RetSyncPoint, ZeEventList, ZeLaunchEvent));

  bool PreferCopyEngine;
  UR_CALL(
      preferCopyEngineForFill(CommandBuffer, PatternSize, PreferCopyEngine));

  ze_command_list_handle_t ZeCommandList =
      CommandBuffer->chooseCommandList(PreferCopyEngine);

  ZE2UR_CALL(zeCommandListAppendMemoryFill,
             (ZeCommandList, Ptr, Pattern, PatternSize, Size, ZeLaunchEvent,
              ZeEventList.size(), getPointerFromVector(ZeEventList)));

  return UR_RESULT_SUCCESS;
}
} // namespace

ur_exp_command_buffer_handle_t_::ur_exp_command_buffer_handle_t_(
    ur_context_handle_t Context, ur_device_handle_t Device,
    ze_command_list_handle_t CommandList,
    ze_command_list_handle_t CommandListTranslated,
    ze_command_list_handle_t CommandListResetEvents,
    ze_command_list_handle_t CopyCommandList, ur_event_handle_t SignalEvent,
    ur_event_handle_t WaitEvent, ur_event_handle_t AllResetEvent,
    const ur_exp_command_buffer_desc_t *Desc, const bool IsInOrderCmdList)
    : Context(Context), Device(Device), ZeComputeCommandList(CommandList),
      ZeComputeCommandListTranslated(CommandListTranslated),
      ZeCommandListResetEvents(CommandListResetEvents),
      ZeCopyCommandList(CopyCommandList), SignalEvent(SignalEvent),
      WaitEvent(WaitEvent), AllResetEvent(AllResetEvent), ZeFencesMap(),
      ZeActiveFence(nullptr), SyncPoints(), NextSyncPoint(0),
      IsUpdatable(Desc ? Desc->isUpdatable : false),
      IsProfilingEnabled(Desc ? Desc->enableProfiling : false),
      IsInOrderCmdList(IsInOrderCmdList) {
  urContextRetain(Context);
  urDeviceRetain(Device);
}

void ur_exp_command_buffer_handle_t_::cleanupCommandBufferResources() {
  // Release the memory allocated to the Context stored in the command_buffer
  urContextRelease(Context);

  // Release the device
  urDeviceRelease(Device);

  // Release the memory allocated to the CommandList stored in the
  // command_buffer
  if (ZeComputeCommandList) {
    ZE_CALL_NOCHECK(zeCommandListDestroy, (ZeComputeCommandList));
  }
  if (useCopyEngine() && ZeCopyCommandList) {
    ZE_CALL_NOCHECK(zeCommandListDestroy, (ZeCopyCommandList));
  }

  // Release the memory allocated to the CommandListResetEvents stored in the
  // command_buffer
  if (ZeCommandListResetEvents) {
    ZE_CALL_NOCHECK(zeCommandListDestroy, (ZeCommandListResetEvents));
  }

  // Release additional signal and wait events used by command_buffer
  if (SignalEvent) {
    CleanupCompletedEvent(SignalEvent, false);
    urEventReleaseInternal(SignalEvent);
  }
  if (WaitEvent) {
    CleanupCompletedEvent(WaitEvent, false);
    urEventReleaseInternal(WaitEvent);
  }
  if (AllResetEvent) {
    CleanupCompletedEvent(AllResetEvent, false);
    urEventReleaseInternal(AllResetEvent);
  }

  // Release events added to the command_buffer
  for (auto &Sync : SyncPoints) {
    auto &Event = Sync.second;
    CleanupCompletedEvent(Event, false);
    urEventReleaseInternal(Event);
  }

  // Release fences allocated to command-buffer
  for (auto &ZeFencePair : ZeFencesMap) {
    auto &ZeFence = ZeFencePair.second;
    ZE_CALL_NOCHECK(zeFenceDestroy, (ZeFence));
  }

  auto ReleaseIndirectMem = [](ur_kernel_handle_t Kernel) {
    if (IndirectAccessTrackingEnabled) {
      // urKernelRelease is called by CleanupCompletedEvent(Event) as soon as
      // kernel execution has finished. This is the place where we need to
      // release memory allocations. If kernel is not in use (not submitted by
      // some other thread) then release referenced memory allocations. As a
      // result, memory can be deallocated and context can be removed from
      // container in the platform. That's why we need to lock a mutex here.
      ur_platform_handle_t Platform = Kernel->Program->Context->getPlatform();
      std::scoped_lock<ur_shared_mutex> ContextsLock(Platform->ContextsMutex);

      if (--Kernel->SubmissionsCount == 0) {
        // Kernel is not submitted for execution, release referenced memory
        // allocations.
        for (auto &MemAlloc : Kernel->MemAllocs) {
          // std::pair<void *const, MemAllocRecord> *, Hash
          USMFreeHelper(MemAlloc->second.Context, MemAlloc->first,
                        MemAlloc->second.OwnNativeHandle);
        }
        Kernel->MemAllocs.clear();
      }
    }
  };

  for (auto &AssociatedKernel : KernelsList) {
    ReleaseIndirectMem(AssociatedKernel);
    urKernelRelease(AssociatedKernel);
  }
}

ur_exp_command_buffer_command_handle_t_::
    ur_exp_command_buffer_command_handle_t_(
        ur_exp_command_buffer_handle_t CommandBuffer, uint64_t CommandId,
        uint32_t WorkDim, bool UserDefinedLocalSize,
        ur_kernel_handle_t Kernel = nullptr)
    : CommandBuffer(CommandBuffer), CommandId(CommandId), WorkDim(WorkDim),
      UserDefinedLocalSize(UserDefinedLocalSize), Kernel(Kernel) {
  urCommandBufferRetainExp(CommandBuffer);
  if (Kernel)
    urKernelRetain(Kernel);
}

ur_exp_command_buffer_command_handle_t_::
    ~ur_exp_command_buffer_command_handle_t_() {
  urCommandBufferReleaseExp(CommandBuffer);
  if (Kernel)
    urKernelRelease(Kernel);
}

void ur_exp_command_buffer_handle_t_::registerSyncPoint(
    ur_exp_command_buffer_sync_point_t SyncPoint, ur_event_handle_t Event) {
  SyncPoints[SyncPoint] = Event;
  NextSyncPoint++;
  ZeEventsList.push_back(Event->ZeEvent);
}

ze_command_list_handle_t
ur_exp_command_buffer_handle_t_::chooseCommandList(bool PreferCopyEngine) {
  if (PreferCopyEngine && this->useCopyEngine() && !this->IsInOrderCmdList) {
    // We indicate that ZeCopyCommandList contains commands to be submitted.
    this->MCopyCommandListEmpty = false;
    return this->ZeCopyCommandList;
  }
  return this->ZeComputeCommandList;
}

ur_result_t ur_exp_command_buffer_handle_t_::getFenceForQueue(
    ze_command_queue_handle_t &ZeCommandQueue, ze_fence_handle_t &ZeFence) {
  // If we already have created a fence for this queue, first reset then reuse
  // it, otherwise create a new fence.
  auto ZeWorkloadFenceForQueue = this->ZeFencesMap.find(ZeCommandQueue);
  if (ZeWorkloadFenceForQueue == this->ZeFencesMap.end()) {
    ZeStruct<ze_fence_desc_t> ZeFenceDesc;
    ZE2UR_CALL(zeFenceCreate, (ZeCommandQueue, &ZeFenceDesc, &ZeFence));
    this->ZeFencesMap.insert({{ZeCommandQueue, ZeFence}});
  } else {
    ZeFence = ZeWorkloadFenceForQueue->second;
    ZE2UR_CALL(zeFenceReset, (ZeFence));
  }
  this->ZeActiveFence = ZeFence;
  return UR_RESULT_SUCCESS;
}

namespace {

/**
 * Creates a L0 command list
 * @param[in] Context The Context associated with the command-list
 * @param[in] Device  The Device associated with the command-list
 * @param[in] IsInOrder Whether the command-list should be in-order.
 * @param[in] IsUpdatable Whether the command-list should be mutable.
 * @param[in] IsCopy Whether to use copy-engine for the the new command-list.
 * @param[out] CommandList The L0 command-list created by this function.
 * @return UR_RESULT_SUCCESS or an error code on failure
 */
ur_result_t createMainCommandList(ur_context_handle_t Context,
                                  ur_device_handle_t Device, bool IsInOrder,
                                  bool IsUpdatable, bool IsCopy,
                                  ze_command_list_handle_t &CommandList) {

  auto Type = IsCopy ? ur_device_handle_t_::queue_group_info_t::type::MainCopy
                     : ur_device_handle_t_::queue_group_info_t::type::Compute;
  uint32_t QueueGroupOrdinal = Device->QueueGroup[Type].ZeOrdinal;

  ZeStruct<ze_command_list_desc_t> ZeCommandListDesc;
  ZeCommandListDesc.commandQueueGroupOrdinal = QueueGroupOrdinal;

  // For non-linear graph, dependencies between commands are explicitly enforced
  // by sync points when enqueuing. Consequently, relax the command ordering in
  // the command list can enable the backend to further optimize the workload
  ZeCommandListDesc.flags = IsInOrder ? ZE_COMMAND_LIST_FLAG_IN_ORDER
                                      : ZE_COMMAND_LIST_FLAG_RELAXED_ORDERING;

  DEBUG_LOG(ZeCommandListDesc.flags);

  ZeStruct<ze_mutable_command_list_exp_desc_t> ZeMutableCommandListDesc;
  if (IsUpdatable) {
    ZeMutableCommandListDesc.flags = 0;
    ZeCommandListDesc.pNext = &ZeMutableCommandListDesc;
  }

  ZE2UR_CALL(zeCommandListCreate, (Context->ZeContext, Device->ZeDevice,
                                   &ZeCommandListDesc, &CommandList));

  return UR_RESULT_SUCCESS;
}

/**
 * Checks whether the command buffer can be constructed using in order
 * command-lists.
 * @param[in] Context The Context associated with the command buffer.
 * @param[in] CommandBufferDesc The description of the command buffer.
 * @return Returns true if in order command-lists can be enabled.
 */
bool canBeInOrder(ur_context_handle_t Context,
                  const ur_exp_command_buffer_desc_t *CommandBufferDesc) {
  // In-order command-lists are not available in old driver version.
  bool CompatibleDriver = isDriverVersionNewerOrSimilar(
      Context->getPlatform()->ZeDriver, 1, 3, L0_DRIVER_INORDER_MIN_VERSION);
  return CompatibleDriver
             ? (CommandBufferDesc ? CommandBufferDesc->isInOrder : false)
             : false;
}
} // namespace

UR_APIEXPORT ur_result_t UR_APICALL
urCommandBufferCreateExp(ur_context_handle_t Context, ur_device_handle_t Device,
                         const ur_exp_command_buffer_desc_t *CommandBufferDesc,
                         ur_exp_command_buffer_handle_t *CommandBuffer) {

  bool IsInOrder = canBeInOrder(Context, CommandBufferDesc);
  bool EnableProfiling =
      CommandBufferDesc && CommandBufferDesc->enableProfiling;
  bool IsUpdatable = CommandBufferDesc && CommandBufferDesc->isUpdatable;

  if (IsUpdatable) {
    UR_ASSERT(Context->getPlatform()->ZeMutableCmdListExt.Supported,
              UR_RESULT_ERROR_UNSUPPORTED_FEATURE);
  }

  ur_event_handle_t SignalEvent;
  ur_event_handle_t WaitEvent;
  ur_event_handle_t AllResetEvent;

  UR_CALL(EventCreate(Context, nullptr, false, false, &SignalEvent, false,
                      !EnableProfiling));
  UR_CALL(EventCreate(Context, nullptr, false, false, &WaitEvent, false,
                      !EnableProfiling));
  UR_CALL(EventCreate(Context, nullptr, false, false, &AllResetEvent, false,
                      !EnableProfiling));
  std::vector<ze_event_handle_t> PrecondEvents = {WaitEvent->ZeEvent,
                                                  AllResetEvent->ZeEvent};

  ze_command_list_handle_t ZeComputeCommandList = nullptr;
  UR_CALL(createMainCommandList(Context, Device, IsInOrder, IsUpdatable, false,
                                ZeComputeCommandList));
  ZE2UR_CALL(zeCommandListAppendBarrier,
             (ZeComputeCommandList, nullptr, PrecondEvents.size(),
              PrecondEvents.data()));

  ze_command_list_handle_t ZeCommandListResetEvents = nullptr;
  UR_CALL(createMainCommandList(Context, Device, false, false, false,
                                ZeCommandListResetEvents));
  ZE2UR_CALL(zeCommandListAppendEventReset,
             (ZeCommandListResetEvents, SignalEvent->ZeEvent));

  // Create a list for copy commands. Note that to simplify the implementation,
  // the current implementation only uses the main copy engine and does not use
  // the link engine even if available.
  ze_command_list_handle_t ZeCopyCommandList = nullptr;
  if (Device->hasMainCopyEngine()) {
    UR_CALL(createMainCommandList(Context, Device, false, false, true,
                                  ZeCopyCommandList));
    ZE2UR_CALL(zeCommandListAppendBarrier,
               (ZeCopyCommandList, nullptr, PrecondEvents.size(),
                PrecondEvents.data()));
  }

  ze_command_list_handle_t ZeComputeCommandListTranslated = nullptr;
  ZE2UR_CALL(zelLoaderTranslateHandle,
             (ZEL_HANDLE_COMMAND_LIST, ZeComputeCommandList,
              (void **)&ZeComputeCommandListTranslated));

  try {
    *CommandBuffer = new ur_exp_command_buffer_handle_t_(
        Context, Device, ZeComputeCommandList, ZeComputeCommandListTranslated,
        ZeCommandListResetEvents, ZeCopyCommandList, SignalEvent, WaitEvent,
        AllResetEvent, CommandBufferDesc, IsInOrder);
  } catch (const std::bad_alloc &) {
    return UR_RESULT_ERROR_OUT_OF_HOST_MEMORY;
  } catch (...) {
    return UR_RESULT_ERROR_UNKNOWN;
  }

  return UR_RESULT_SUCCESS;
}

UR_APIEXPORT ur_result_t UR_APICALL
urCommandBufferRetainExp(ur_exp_command_buffer_handle_t CommandBuffer) {
  CommandBuffer->RefCount.increment();
  return UR_RESULT_SUCCESS;
}

UR_APIEXPORT ur_result_t UR_APICALL
urCommandBufferReleaseExp(ur_exp_command_buffer_handle_t CommandBuffer) {
  if (!CommandBuffer->RefCount.decrementAndTest())
    return UR_RESULT_SUCCESS;

  CommandBuffer->cleanupCommandBufferResources();
  delete CommandBuffer;
  return UR_RESULT_SUCCESS;
}

UR_APIEXPORT ur_result_t UR_APICALL
urCommandBufferFinalizeExp(ur_exp_command_buffer_handle_t CommandBuffer) {
  UR_ASSERT(CommandBuffer, UR_RESULT_ERROR_INVALID_NULL_POINTER);
  // It is not allowed to append to command list from multiple threads.
  std::scoped_lock<ur_shared_mutex> Guard(CommandBuffer->Mutex);

  if (CommandBuffer->IsInOrderCmdList) {
    ZE2UR_CALL(zeCommandListAppendSignalEvent,
               (CommandBuffer->ZeComputeCommandList,
                CommandBuffer->SignalEvent->ZeEvent));
  } else {
    // Reset the L0 events we use for command-buffer sync-points to the
    // non-signaled state. This is required for multiple submissions.
    for (auto &Event : CommandBuffer->ZeEventsList) {
      ZE2UR_CALL(zeCommandListAppendEventReset,
                 (CommandBuffer->ZeCommandListResetEvents, Event));
    }

    // Wait for all the user added commands to complete, and signal the
    // command-buffer signal-event when they are done.
    ZE2UR_CALL(zeCommandListAppendBarrier,
               (CommandBuffer->ZeComputeCommandList,
                CommandBuffer->SignalEvent->ZeEvent,
                CommandBuffer->ZeEventsList.size(),
                CommandBuffer->ZeEventsList.data()));
  }

  ZE2UR_CALL(zeCommandListAppendSignalEvent,
             (CommandBuffer->ZeCommandListResetEvents,
              CommandBuffer->AllResetEvent->ZeEvent));

  // Close the command lists and have them ready for dispatch.
  ZE2UR_CALL(zeCommandListClose, (CommandBuffer->ZeComputeCommandList));
  ZE2UR_CALL(zeCommandListClose, (CommandBuffer->ZeCommandListResetEvents));

  if (CommandBuffer->useCopyEngine()) {
    ZE2UR_CALL(zeCommandListClose, (CommandBuffer->ZeCopyCommandList));
  }

  CommandBuffer->IsFinalized = true;

  return UR_RESULT_SUCCESS;
}

namespace {

/**
 * Sets the global offset for a kernel command that will be appended to the
 * command buffer.
 * @param[in] CommandBuffer The CommandBuffer where the command will be
 * appended.
 * @param[in] Kernel The handle to the kernel that will be appended.
 * @param[in] GlobalWorkOffset The global offset value.
 * @return UR_RESULT_SUCCESS or an error code on failure
 */
ur_result_t setKernelGlobalOffset(ur_exp_command_buffer_handle_t CommandBuffer,
                                  ur_kernel_handle_t Kernel,
                                  const size_t *GlobalWorkOffset) {

  if (!CommandBuffer->Context->getPlatform()
           ->ZeDriverGlobalOffsetExtensionFound) {
    logger::debug("No global offset extension found on this driver");
    return UR_RESULT_ERROR_INVALID_VALUE;
  }

  ZE2UR_CALL(zeKernelSetGlobalOffsetExp,
             (Kernel->ZeKernel, GlobalWorkOffset[0], GlobalWorkOffset[1],
              GlobalWorkOffset[2]));

  return UR_RESULT_SUCCESS;
}

/**
 * Sets the kernel arguments for a kernel command that will be appended to the
 * command buffer.
 * @param[in] CommandBuffer The CommandBuffer where the command will be
 * appended.
 * @param[in] Kernel The handle to the kernel that will be appended.
 * @return UR_RESULT_SUCCESS or an error code on failure
 */
ur_result_t
setKernelPendingArguments(ur_exp_command_buffer_handle_t CommandBuffer,
                          ur_kernel_handle_t Kernel) {

  // If there are any pending arguments set them now.
  for (auto &Arg : Kernel->PendingArguments) {
    // The ArgValue may be a NULL pointer in which case a NULL value is used for
    // the kernel argument declared as a pointer to global or constant memory.
    char **ZeHandlePtr = nullptr;
    if (Arg.Value) {
      UR_CALL(Arg.Value->getZeHandlePtr(ZeHandlePtr, Arg.AccessMode,
                                        CommandBuffer->Device));
    }
    ZE2UR_CALL(zeKernelSetArgumentValue,
               (Kernel->ZeKernel, Arg.Index, Arg.Size, ZeHandlePtr));
  }
  Kernel->PendingArguments.clear();

  return UR_RESULT_SUCCESS;
}

/**
 * Creates a new command handle to use in future updates to the command buffer.
 * @param[in] CommandBuffer The CommandBuffer associated with the new command.
 * @param[in] Kernel  The Kernel associated with the new command.
 * @param[in] WorkDim Dimensions of the kernel associated with the new command.
 * @param[in] LocalWorkSize LocalWorkSize of the kernel associated with the new
 * command.
 * @param[out] Command The handle to the new command.
 * @return UR_RESULT_SUCCESS or an error code on failure
 */
ur_result_t
createCommandHandle(ur_exp_command_buffer_handle_t CommandBuffer,
                    ur_kernel_handle_t Kernel, uint32_t WorkDim,
                    const size_t *LocalWorkSize,
                    ur_exp_command_buffer_command_handle_t &Command) {

  assert(CommandBuffer->IsUpdatable);

  // If command-buffer is updatable then get command id which is going to be
  // used if command is updated in the future. This
  // zeCommandListGetNextCommandIdExp can be called only if the command is
  // updatable.
  uint64_t CommandId = 0;
  ZeStruct<ze_mutable_command_id_exp_desc_t> ZeMutableCommandDesc;
  ZeMutableCommandDesc.flags = ZE_MUTABLE_COMMAND_EXP_FLAG_KERNEL_ARGUMENTS |
                               ZE_MUTABLE_COMMAND_EXP_FLAG_GROUP_COUNT |
                               ZE_MUTABLE_COMMAND_EXP_FLAG_GROUP_SIZE |
                               ZE_MUTABLE_COMMAND_EXP_FLAG_GLOBAL_OFFSET;

  auto Platform = CommandBuffer->Context->getPlatform();
  ZE2UR_CALL(Platform->ZeMutableCmdListExt.zexCommandListGetNextCommandIdExp,
             (CommandBuffer->ZeComputeCommandListTranslated,
              &ZeMutableCommandDesc, &CommandId));
  DEBUG_LOG(CommandId);

  try {
    Command = new ur_exp_command_buffer_command_handle_t_(
        CommandBuffer, CommandId, WorkDim, LocalWorkSize != nullptr, Kernel);
  } catch (const std::bad_alloc &) {
    return UR_RESULT_ERROR_OUT_OF_HOST_MEMORY;
  } catch (...) {
    return UR_RESULT_ERROR_UNKNOWN;
  }

  return UR_RESULT_SUCCESS;
}
} // namespace

UR_APIEXPORT ur_result_t UR_APICALL urCommandBufferAppendKernelLaunchExp(
    ur_exp_command_buffer_handle_t CommandBuffer, ur_kernel_handle_t Kernel,
    uint32_t WorkDim, const size_t *GlobalWorkOffset,
    const size_t *GlobalWorkSize, const size_t *LocalWorkSize,
    uint32_t NumSyncPointsInWaitList,
    const ur_exp_command_buffer_sync_point_t *SyncPointWaitList,
    ur_exp_command_buffer_sync_point_t *RetSyncPoint,
    ur_exp_command_buffer_command_handle_t *Command) {
  UR_ASSERT(Kernel->Program, UR_RESULT_ERROR_INVALID_NULL_POINTER);

  // Lock automatically releases when this goes out of scope.
  std::scoped_lock<ur_shared_mutex, ur_shared_mutex, ur_shared_mutex> Lock(
      Kernel->Mutex, Kernel->Program->Mutex, CommandBuffer->Mutex);

  if (GlobalWorkOffset != NULL) {
    UR_CALL(setKernelGlobalOffset(CommandBuffer, Kernel, GlobalWorkOffset));
  }

  // If there are any pending arguments set them now.
  if (!Kernel->PendingArguments.empty()) {
    UR_CALL(setKernelPendingArguments(CommandBuffer, Kernel));
  }

  ze_group_count_t ZeThreadGroupDimensions{1, 1, 1};
  uint32_t WG[3];
  UR_CALL(calculateKernelWorkDimensions(Kernel->ZeKernel, CommandBuffer->Device,
                                        ZeThreadGroupDimensions, WG, WorkDim,
                                        GlobalWorkSize, LocalWorkSize));

  ZE2UR_CALL(zeKernelSetGroupSize, (Kernel->ZeKernel, WG[0], WG[1], WG[2]));

  CommandBuffer->KernelsList.push_back(Kernel);

  // Increment the reference count of the Kernel and indicate that the Kernel
  // is in use. Once the event has been signaled, the code in
  // CleanupCompletedEvent(Event) will do a urKernelRelease to update the
  // reference count on the kernel, using the kernel saved in CommandData.
  UR_CALL(urKernelRetain(Kernel));

  if (Command && CommandBuffer->IsUpdatable) {
    UR_CALL(createCommandHandle(CommandBuffer, Kernel, WorkDim, LocalWorkSize,
                                *Command));
  }

  std::vector<ze_event_handle_t> ZeEventList;
  ze_event_handle_t ZeLaunchEvent = nullptr;
  UR_CALL(createSyncPointAndGetZeEvents(
      UR_COMMAND_KERNEL_LAUNCH, CommandBuffer, NumSyncPointsInWaitList,
      SyncPointWaitList, false, RetSyncPoint, ZeEventList, ZeLaunchEvent));

  ZE2UR_CALL(zeCommandListAppendLaunchKernel,
             (CommandBuffer->ZeComputeCommandList, Kernel->ZeKernel,
              &ZeThreadGroupDimensions, ZeLaunchEvent, ZeEventList.size(),
              getPointerFromVector(ZeEventList)));

  return UR_RESULT_SUCCESS;
}

UR_APIEXPORT ur_result_t UR_APICALL urCommandBufferAppendUSMMemcpyExp(
    ur_exp_command_buffer_handle_t CommandBuffer, void *Dst, const void *Src,
    size_t Size, uint32_t NumSyncPointsInWaitList,
    const ur_exp_command_buffer_sync_point_t *SyncPointWaitList,
    ur_exp_command_buffer_sync_point_t *SyncPoint) {

  bool PreferCopyEngine = !IsDevicePointer(CommandBuffer->Context, Src) ||
                          !IsDevicePointer(CommandBuffer->Context, Dst);
  // For better performance, Copy Engines are not preferred given Shared
  // pointers on DG2.
  if (CommandBuffer->Device->isDG2() &&
      (IsSharedPointer(CommandBuffer->Context, Src) ||
       IsSharedPointer(CommandBuffer->Context, Dst))) {
    PreferCopyEngine = false;
  }
  PreferCopyEngine |= UseCopyEngineForD2DCopy;

  return enqueueCommandBufferMemCopyHelper(
      UR_COMMAND_USM_MEMCPY, CommandBuffer, Dst, Src, Size, PreferCopyEngine,
      NumSyncPointsInWaitList, SyncPointWaitList, SyncPoint);
}

UR_APIEXPORT ur_result_t UR_APICALL urCommandBufferAppendMemBufferCopyExp(
    ur_exp_command_buffer_handle_t CommandBuffer, ur_mem_handle_t SrcMem,
    ur_mem_handle_t DstMem, size_t SrcOffset, size_t DstOffset, size_t Size,
    uint32_t NumSyncPointsInWaitList,
    const ur_exp_command_buffer_sync_point_t *SyncPointWaitList,
    ur_exp_command_buffer_sync_point_t *SyncPoint) {
  auto SrcBuffer = ur_cast<_ur_buffer *>(SrcMem);
  auto DstBuffer = ur_cast<_ur_buffer *>(DstMem);

  std::shared_lock<ur_shared_mutex> SrcLock(SrcBuffer->Mutex, std::defer_lock);
  std::scoped_lock<std::shared_lock<ur_shared_mutex>, ur_shared_mutex> LockAll(
      SrcLock, DstBuffer->Mutex);

  char *ZeHandleSrc;
  UR_CALL(SrcBuffer->getZeHandle(ZeHandleSrc, ur_mem_handle_t_::read_only,
                                 CommandBuffer->Device));
  char *ZeHandleDst;
  UR_CALL(DstBuffer->getZeHandle(ZeHandleDst, ur_mem_handle_t_::write_only,
                                 CommandBuffer->Device));

  bool PreferCopyEngine = (SrcBuffer->OnHost || DstBuffer->OnHost);

  PreferCopyEngine |= UseCopyEngineForD2DCopy;

  return enqueueCommandBufferMemCopyHelper(
      UR_COMMAND_MEM_BUFFER_COPY, CommandBuffer, ZeHandleDst + DstOffset,
      ZeHandleSrc + SrcOffset, Size, PreferCopyEngine, NumSyncPointsInWaitList,
      SyncPointWaitList, SyncPoint);
}

UR_APIEXPORT ur_result_t UR_APICALL urCommandBufferAppendMemBufferCopyRectExp(
    ur_exp_command_buffer_handle_t CommandBuffer, ur_mem_handle_t SrcMem,
    ur_mem_handle_t DstMem, ur_rect_offset_t SrcOrigin,
    ur_rect_offset_t DstOrigin, ur_rect_region_t Region, size_t SrcRowPitch,
    size_t SrcSlicePitch, size_t DstRowPitch, size_t DstSlicePitch,
    uint32_t NumSyncPointsInWaitList,
    const ur_exp_command_buffer_sync_point_t *SyncPointWaitList,
    ur_exp_command_buffer_sync_point_t *SyncPoint) {
  auto SrcBuffer = ur_cast<_ur_buffer *>(SrcMem);
  auto DstBuffer = ur_cast<_ur_buffer *>(DstMem);

  std::shared_lock<ur_shared_mutex> SrcLock(SrcBuffer->Mutex, std::defer_lock);
  std::scoped_lock<std::shared_lock<ur_shared_mutex>, ur_shared_mutex> LockAll(
      SrcLock, DstBuffer->Mutex);

  char *ZeHandleSrc;
  UR_CALL(SrcBuffer->getZeHandle(ZeHandleSrc, ur_mem_handle_t_::read_only,
                                 CommandBuffer->Device));
  char *ZeHandleDst;
  UR_CALL(DstBuffer->getZeHandle(ZeHandleDst, ur_mem_handle_t_::write_only,
                                 CommandBuffer->Device));

  bool PreferCopyEngine = (SrcBuffer->OnHost || DstBuffer->OnHost);

  PreferCopyEngine |= UseCopyEngineForD2DCopy;

  return enqueueCommandBufferMemCopyRectHelper(
      UR_COMMAND_MEM_BUFFER_COPY_RECT, CommandBuffer, ZeHandleDst, ZeHandleSrc,
      SrcOrigin, DstOrigin, Region, SrcRowPitch, DstRowPitch, SrcSlicePitch,
      DstSlicePitch, PreferCopyEngine, NumSyncPointsInWaitList,
      SyncPointWaitList, SyncPoint);
}

UR_APIEXPORT ur_result_t UR_APICALL urCommandBufferAppendMemBufferWriteExp(
    ur_exp_command_buffer_handle_t CommandBuffer, ur_mem_handle_t Buffer,
    size_t Offset, size_t Size, const void *Src,
    uint32_t NumSyncPointsInWaitList,
    const ur_exp_command_buffer_sync_point_t *SyncPointWaitList,
    ur_exp_command_buffer_sync_point_t *SyncPoint) {
  std::scoped_lock<ur_shared_mutex> Lock(Buffer->Mutex);

  char *ZeHandleDst = nullptr;
  UR_CALL(Buffer->getZeHandle(ZeHandleDst, ur_mem_handle_t_::write_only,
                              CommandBuffer->Device));
  // Always prefer copy engine for writes
  bool PreferCopyEngine = true;

  return enqueueCommandBufferMemCopyHelper(
      UR_COMMAND_MEM_BUFFER_WRITE, CommandBuffer,
      ZeHandleDst + Offset, // dst
      Src,                  // src
      Size, PreferCopyEngine, NumSyncPointsInWaitList, SyncPointWaitList,
      SyncPoint);
}

UR_APIEXPORT ur_result_t UR_APICALL urCommandBufferAppendMemBufferWriteRectExp(
    ur_exp_command_buffer_handle_t CommandBuffer, ur_mem_handle_t Buffer,
    ur_rect_offset_t BufferOffset, ur_rect_offset_t HostOffset,
    ur_rect_region_t Region, size_t BufferRowPitch, size_t BufferSlicePitch,
    size_t HostRowPitch, size_t HostSlicePitch, void *Src,
    uint32_t NumSyncPointsInWaitList,
    const ur_exp_command_buffer_sync_point_t *SyncPointWaitList,
    ur_exp_command_buffer_sync_point_t *SyncPoint) {
  std::scoped_lock<ur_shared_mutex> Lock(Buffer->Mutex);

  char *ZeHandleDst = nullptr;
  UR_CALL(Buffer->getZeHandle(ZeHandleDst, ur_mem_handle_t_::write_only,
                              CommandBuffer->Device));

  // Always prefer copy engine for writes
  bool PreferCopyEngine = true;

  return enqueueCommandBufferMemCopyRectHelper(
      UR_COMMAND_MEM_BUFFER_WRITE_RECT, CommandBuffer, ZeHandleDst,
      const_cast<char *>(static_cast<const char *>(Src)), HostOffset,
      BufferOffset, Region, HostRowPitch, BufferRowPitch, HostSlicePitch,
      BufferSlicePitch, PreferCopyEngine, NumSyncPointsInWaitList,
      SyncPointWaitList, SyncPoint);
}

UR_APIEXPORT ur_result_t UR_APICALL urCommandBufferAppendMemBufferReadExp(
    ur_exp_command_buffer_handle_t CommandBuffer, ur_mem_handle_t Buffer,
    size_t Offset, size_t Size, void *Dst, uint32_t NumSyncPointsInWaitList,
    const ur_exp_command_buffer_sync_point_t *SyncPointWaitList,
    ur_exp_command_buffer_sync_point_t *SyncPoint) {
  std::scoped_lock<ur_shared_mutex> SrcLock(Buffer->Mutex);

  char *ZeHandleSrc = nullptr;
  UR_CALL(Buffer->getZeHandle(ZeHandleSrc, ur_mem_handle_t_::read_only,
                              CommandBuffer->Device));

  // Always prefer copy engine for reads
  bool PreferCopyEngine = true;

  return enqueueCommandBufferMemCopyHelper(
      UR_COMMAND_MEM_BUFFER_READ, CommandBuffer, Dst, ZeHandleSrc + Offset,
      Size, PreferCopyEngine, NumSyncPointsInWaitList, SyncPointWaitList,
      SyncPoint);
}

UR_APIEXPORT ur_result_t UR_APICALL urCommandBufferAppendMemBufferReadRectExp(
    ur_exp_command_buffer_handle_t CommandBuffer, ur_mem_handle_t Buffer,
    ur_rect_offset_t BufferOffset, ur_rect_offset_t HostOffset,
    ur_rect_region_t Region, size_t BufferRowPitch, size_t BufferSlicePitch,
    size_t HostRowPitch, size_t HostSlicePitch, void *Dst,
    uint32_t NumSyncPointsInWaitList,
    const ur_exp_command_buffer_sync_point_t *SyncPointWaitList,
    ur_exp_command_buffer_sync_point_t *SyncPoint) {
  std::scoped_lock<ur_shared_mutex> SrcLock(Buffer->Mutex);

  char *ZeHandleSrc;
  UR_CALL(Buffer->getZeHandle(ZeHandleSrc, ur_mem_handle_t_::read_only,
                              CommandBuffer->Device));

  // Always prefer copy engine for reads
  bool PreferCopyEngine = true;

  return enqueueCommandBufferMemCopyRectHelper(
      UR_COMMAND_MEM_BUFFER_READ_RECT, CommandBuffer, Dst, ZeHandleSrc,
      BufferOffset, HostOffset, Region, BufferRowPitch, HostRowPitch,
      BufferSlicePitch, HostSlicePitch, PreferCopyEngine,
      NumSyncPointsInWaitList, SyncPointWaitList, SyncPoint);
}

UR_APIEXPORT ur_result_t UR_APICALL urCommandBufferAppendUSMPrefetchExp(
    ur_exp_command_buffer_handle_t CommandBuffer, const void *Mem, size_t Size,
    ur_usm_migration_flags_t Flags, uint32_t NumSyncPointsInWaitList,
    const ur_exp_command_buffer_sync_point_t *SyncPointWaitList,
    ur_exp_command_buffer_sync_point_t *RetSyncPoint) {
  std::ignore = Flags;

  if (CommandBuffer->IsInOrderCmdList) {
    // Add the prefetch command to the command buffer.
    // Note that L0 does not handle migration flags.
    ZE2UR_CALL(zeCommandListAppendMemoryPrefetch,
               (CommandBuffer->ZeComputeCommandList, Mem, Size));
  } else {
    std::vector<ze_event_handle_t> ZeEventList;
    ze_event_handle_t ZeLaunchEvent = nullptr;
    UR_CALL(createSyncPointAndGetZeEvents(
        UR_COMMAND_USM_PREFETCH, CommandBuffer, NumSyncPointsInWaitList,
        SyncPointWaitList, true, RetSyncPoint, ZeEventList, ZeLaunchEvent));

    if (NumSyncPointsInWaitList) {
      ZE2UR_CALL(zeCommandListAppendWaitOnEvents,
                 (CommandBuffer->ZeComputeCommandList, NumSyncPointsInWaitList,
                  ZeEventList.data()));
    }

    // Add the prefetch command to the command buffer.
    // Note that L0 does not handle migration flags.
    ZE2UR_CALL(zeCommandListAppendMemoryPrefetch,
               (CommandBuffer->ZeComputeCommandList, Mem, Size));

    // Level Zero does not have a completion "event" with the prefetch API,
    // so manually add command to signal our event.
    ZE2UR_CALL(zeCommandListAppendSignalEvent,
               (CommandBuffer->ZeComputeCommandList, ZeLaunchEvent));
  }

  return UR_RESULT_SUCCESS;
}

UR_APIEXPORT ur_result_t UR_APICALL urCommandBufferAppendUSMAdviseExp(
    ur_exp_command_buffer_handle_t CommandBuffer, const void *Mem, size_t Size,
    ur_usm_advice_flags_t Advice, uint32_t NumSyncPointsInWaitList,
    const ur_exp_command_buffer_sync_point_t *SyncPointWaitList,
    ur_exp_command_buffer_sync_point_t *RetSyncPoint) {
  // A memory chunk can be advised with muliple memory advices
  // We therefore prefer if statements to switch cases to combine all potential
  // flags
  uint32_t Value = 0;
  if (Advice & UR_USM_ADVICE_FLAG_SET_READ_MOSTLY)
    Value |= static_cast<int>(ZE_MEMORY_ADVICE_SET_READ_MOSTLY);
  if (Advice & UR_USM_ADVICE_FLAG_CLEAR_READ_MOSTLY)
    Value |= static_cast<int>(ZE_MEMORY_ADVICE_CLEAR_READ_MOSTLY);
  if (Advice & UR_USM_ADVICE_FLAG_SET_PREFERRED_LOCATION)
    Value |= static_cast<int>(ZE_MEMORY_ADVICE_SET_PREFERRED_LOCATION);
  if (Advice & UR_USM_ADVICE_FLAG_CLEAR_PREFERRED_LOCATION)
    Value |= static_cast<int>(ZE_MEMORY_ADVICE_CLEAR_PREFERRED_LOCATION);
  if (Advice & UR_USM_ADVICE_FLAG_SET_NON_ATOMIC_MOSTLY)
    Value |= static_cast<int>(ZE_MEMORY_ADVICE_SET_NON_ATOMIC_MOSTLY);
  if (Advice & UR_USM_ADVICE_FLAG_CLEAR_NON_ATOMIC_MOSTLY)
    Value |= static_cast<int>(ZE_MEMORY_ADVICE_CLEAR_NON_ATOMIC_MOSTLY);
  if (Advice & UR_USM_ADVICE_FLAG_BIAS_CACHED)
    Value |= static_cast<int>(ZE_MEMORY_ADVICE_BIAS_CACHED);
  if (Advice & UR_USM_ADVICE_FLAG_BIAS_UNCACHED)
    Value |= static_cast<int>(ZE_MEMORY_ADVICE_BIAS_UNCACHED);
  if (Advice & UR_USM_ADVICE_FLAG_SET_PREFERRED_LOCATION_HOST)
    Value |= static_cast<int>(ZE_MEMORY_ADVICE_SET_PREFERRED_LOCATION);
  if (Advice & UR_USM_ADVICE_FLAG_CLEAR_PREFERRED_LOCATION_HOST)
    Value |= static_cast<int>(ZE_MEMORY_ADVICE_CLEAR_PREFERRED_LOCATION);

  ze_memory_advice_t ZeAdvice = static_cast<ze_memory_advice_t>(Value);

  if (CommandBuffer->IsInOrderCmdList) {
    ZE2UR_CALL(zeCommandListAppendMemAdvise,
               (CommandBuffer->ZeComputeCommandList,
                CommandBuffer->Device->ZeDevice, Mem, Size, ZeAdvice));
  } else {
    std::vector<ze_event_handle_t> ZeEventList;
    ze_event_handle_t ZeLaunchEvent = nullptr;
    UR_CALL(createSyncPointAndGetZeEvents(
        UR_COMMAND_USM_ADVISE, CommandBuffer, NumSyncPointsInWaitList,
        SyncPointWaitList, true, RetSyncPoint, ZeEventList, ZeLaunchEvent));

    if (NumSyncPointsInWaitList) {
      ZE2UR_CALL(zeCommandListAppendWaitOnEvents,
                 (CommandBuffer->ZeComputeCommandList, NumSyncPointsInWaitList,
                  ZeEventList.data()));
    }

    ZE2UR_CALL(zeCommandListAppendMemAdvise,
               (CommandBuffer->ZeComputeCommandList,
                CommandBuffer->Device->ZeDevice, Mem, Size, ZeAdvice));

    // Level Zero does not have a completion "event" with the advise API,
    // so manually add command to signal our event.
    ZE2UR_CALL(zeCommandListAppendSignalEvent,
               (CommandBuffer->ZeComputeCommandList, ZeLaunchEvent));
  }

  return UR_RESULT_SUCCESS;
}

UR_APIEXPORT ur_result_t UR_APICALL urCommandBufferAppendMemBufferFillExp(
    ur_exp_command_buffer_handle_t CommandBuffer, ur_mem_handle_t Buffer,
    const void *Pattern, size_t PatternSize, size_t Offset, size_t Size,
    uint32_t NumSyncPointsInWaitList,
    const ur_exp_command_buffer_sync_point_t *SyncPointWaitList,
    ur_exp_command_buffer_sync_point_t *SyncPoint) {

  std::scoped_lock<ur_shared_mutex> Lock(Buffer->Mutex);

  char *ZeHandleDst = nullptr;
  _ur_buffer *UrBuffer = reinterpret_cast<_ur_buffer *>(Buffer);
  UR_CALL(UrBuffer->getZeHandle(ZeHandleDst, ur_mem_handle_t_::write_only,
                                CommandBuffer->Device));

  return enqueueCommandBufferFillHelper(
      UR_COMMAND_MEM_BUFFER_FILL, CommandBuffer, ZeHandleDst + Offset,
      Pattern,     // It will be interpreted as an 8-bit value,
      PatternSize, // which is indicated with this pattern_size==1
      Size, NumSyncPointsInWaitList, SyncPointWaitList, SyncPoint);
}

UR_APIEXPORT ur_result_t UR_APICALL urCommandBufferAppendUSMFillExp(
    ur_exp_command_buffer_handle_t CommandBuffer, void *Ptr,
    const void *Pattern, size_t PatternSize, size_t Size,
    uint32_t NumSyncPointsInWaitList,
    const ur_exp_command_buffer_sync_point_t *SyncPointWaitList,
    ur_exp_command_buffer_sync_point_t *SyncPoint) {

  return enqueueCommandBufferFillHelper(
      UR_COMMAND_MEM_BUFFER_FILL, CommandBuffer, Ptr,
      Pattern,     // It will be interpreted as an 8-bit value,
      PatternSize, // which is indicated with this pattern_size==1
      Size, NumSyncPointsInWaitList, SyncPointWaitList, SyncPoint);
}

namespace {

/**
 * Gets an L0 command queue that supports the chosen engine.
 * @param[in] Queue The UR queue used to submit the command buffer.
 * @param[in] UseCopyEngine Which engine to use. true for the copy engine and
 * false for the compute engine.
 * @param[out] ZeCommandQueue The L0 command queue.
 * @return UR_RESULT_SUCCESS or an error code on failure
 */
ur_result_t getZeCommandQueue(ur_queue_handle_legacy_t Queue,
                              bool UseCopyEngine,
                              ze_command_queue_handle_t &ZeCommandQueue) {
  auto &QGroup = Queue->getQueueGroup(UseCopyEngine);
  uint32_t QueueGroupOrdinal;
  ZeCommandQueue = QGroup.getZeQueue(&QueueGroupOrdinal);
  return UR_RESULT_SUCCESS;
}

/**
 * Waits for the all the dependencies of the command buffer
 * @param[in] CommandBuffer The command buffer.
 * @param[in] Queue The UR queue used to submit the command buffer.
 * @param[in] NumEventsInWaitList The number of events to wait for.
 * @param[in] EventWaitList List of events to wait for.
 * @return UR_RESULT_SUCCESS or an error code on failure
 */
ur_result_t waitForDependencies(ur_exp_command_buffer_handle_t CommandBuffer,
                                ur_queue_handle_legacy_t Queue,
                                uint32_t NumEventsInWaitList,
                                const ur_event_handle_t *EventWaitList) {
  const bool UseCopyEngine = false;
  bool MustSignalWaitEvent = true;
  if (NumEventsInWaitList) {
    _ur_ze_event_list_t TmpWaitList;
    UR_CALL(TmpWaitList.createAndRetainUrZeEventList(
        NumEventsInWaitList, EventWaitList, Queue, UseCopyEngine));

    // Update the WaitList of the Wait Event
    // Events are appended to the WaitList if the WaitList is not empty
    if (CommandBuffer->WaitEvent->WaitList.isEmpty())
      CommandBuffer->WaitEvent->WaitList = TmpWaitList;
    else
      CommandBuffer->WaitEvent->WaitList.insert(TmpWaitList);

    if (!CommandBuffer->WaitEvent->WaitList.isEmpty()) {
      // Create command-list to execute before `CommandListPtr` and will signal
      // when `EventWaitList` dependencies are complete.
      ur_command_list_ptr_t WaitCommandList{};
      UR_CALL(Queue->Context->getAvailableCommandList(
          Queue, WaitCommandList, false, NumEventsInWaitList, EventWaitList,
          false));

      ZE2UR_CALL(zeCommandListAppendBarrier,
                 (WaitCommandList->first, CommandBuffer->WaitEvent->ZeEvent,
                  CommandBuffer->WaitEvent->WaitList.Length,
                  CommandBuffer->WaitEvent->WaitList.ZeEventList));
      Queue->executeCommandList(WaitCommandList, false, false);
      MustSignalWaitEvent = false;
    }
  }
  // Given WaitEvent was created without specifying Counting Events, then this
  // event can be signalled on the host.
  if (MustSignalWaitEvent) {
    ZE2UR_CALL(zeEventHostSignal, (CommandBuffer->WaitEvent->ZeEvent));
  }
  return UR_RESULT_SUCCESS;
}

/**
 * Creates a host visible event and appends a barrier to signal it when the
 * command buffer finishes executing.
 * @param[in] CommandBuffer The command buffer.
 * @param[in] Queue The UR queue used to submit the command buffer.
 * @param[in] SignalCommandList The command-list to append the barrier to.
 * @param[out] Event The host visible event which will be returned to the user.
 * @return UR_RESULT_SUCCESS or an error code on failure
 */
ur_result_t createUserEvent(ur_exp_command_buffer_handle_t CommandBuffer,
                            ur_queue_handle_legacy_t Queue,
                            ur_command_list_ptr_t SignalCommandList,
                            ur_event_handle_t &Event) {
  // Execution event for this enqueue of the UR command-buffer
  ur_event_handle_t RetEvent{};

  UR_CALL(createEventAndAssociateQueue(Queue, &RetEvent,
                                       UR_COMMAND_COMMAND_BUFFER_ENQUEUE_EXP,
                                       SignalCommandList, false, false, true));

  if ((Queue->Properties & UR_QUEUE_FLAG_PROFILING_ENABLE) &&
      (!CommandBuffer->IsInOrderCmdList) &&
      (CommandBuffer->IsProfilingEnabled)) {
    // Multiple submissions of a command buffer implies that we need to save
    // the event timestamps before resubmiting the command buffer. We
    // therefore copy the these timestamps in a dedicated USM memory section
    // before completing the command buffer execution, and then attach this
    // memory to the event returned to users to allow to allow the profiling
    // engine to recover these timestamps.
    command_buffer_profiling_t *Profiling = new command_buffer_profiling_t();

    Profiling->NumEvents = CommandBuffer->ZeEventsList.size();
    Profiling->Timestamps =
        new ze_kernel_timestamp_result_t[Profiling->NumEvents];

    ZE2UR_CALL(zeCommandListAppendQueryKernelTimestamps,
               (SignalCommandList->first, CommandBuffer->ZeEventsList.size(),
                CommandBuffer->ZeEventsList.data(),
                (void *)Profiling->Timestamps, 0, RetEvent->ZeEvent, 1,
                &(CommandBuffer->SignalEvent->ZeEvent)));

    RetEvent->CommandData = static_cast<void *>(Profiling);
  } else {
    ZE2UR_CALL(zeCommandListAppendBarrier,
               (SignalCommandList->first, RetEvent->ZeEvent, 1,
                &(CommandBuffer->SignalEvent->ZeEvent)));
  }

  Event = RetEvent;

  return UR_RESULT_SUCCESS;
}
} // namespace

UR_APIEXPORT ur_result_t UR_APICALL urCommandBufferEnqueueExp(
    ur_exp_command_buffer_handle_t CommandBuffer, ur_queue_handle_t UrQueue,
    uint32_t NumEventsInWaitList, const ur_event_handle_t *EventWaitList,
    ur_event_handle_t *Event) {
  auto Queue = Legacy(UrQueue);
  std::scoped_lock<ur_shared_mutex> Lock(Queue->Mutex);

  ze_command_queue_handle_t ZeCommandQueue;
  getZeCommandQueue(Queue, false, ZeCommandQueue);

  ze_fence_handle_t ZeFence;
  CommandBuffer->getFenceForQueue(ZeCommandQueue, ZeFence);

  UR_CALL(waitForDependencies(CommandBuffer, Queue, NumEventsInWaitList,
                              EventWaitList));

  // Submit reset events command-list. This command-list is of a batch
  // command-list type, regardless of the UR Queue type. We therefore need to
  // submit the list directly using the Level-Zero API to avoid type mismatches
  // if using UR functions.
  ZE2UR_CALL(
      zeCommandQueueExecuteCommandLists,
      (ZeCommandQueue, 1, &CommandBuffer->ZeCommandListResetEvents, nullptr));

  // Submit main command-list. This command-list is of a batch command-list
  // type, regardless of the UR Queue type. We therefore need to submit the list
  // directly using the Level-Zero API to avoid type mismatches if using UR
  // functions.
  ZE2UR_CALL(
      zeCommandQueueExecuteCommandLists,
      (ZeCommandQueue, 1, &CommandBuffer->ZeComputeCommandList, ZeFence));

  // The Copy command-list is submitted to the main copy queue if it is not
  // empty.
  if (!CommandBuffer->MCopyCommandListEmpty) {
    ze_command_queue_handle_t ZeCopyCommandQueue;
    getZeCommandQueue(Queue, true, ZeCopyCommandQueue);
    ZE2UR_CALL(
        zeCommandQueueExecuteCommandLists,
        (ZeCopyCommandQueue, 1, &CommandBuffer->ZeCopyCommandList, nullptr));
  }

  // Create a command-list to signal the Event on completion
  ur_command_list_ptr_t SignalCommandList{};
  UR_CALL(Queue->Context->getAvailableCommandList(Queue, SignalCommandList,
                                                  false, NumEventsInWaitList,
                                                  EventWaitList, false));

  // Reset the wait-event for the UR command-buffer that is signaled when its
  // submission dependencies have been satisfied.
  ZE2UR_CALL(zeCommandListAppendEventReset,
             (SignalCommandList->first, CommandBuffer->WaitEvent->ZeEvent));
  // Reset the all-reset-event for the UR command-buffer that is signaled when
  // all events of the main command-list have been reset.
  ZE2UR_CALL(zeCommandListAppendEventReset,
             (SignalCommandList->first, CommandBuffer->AllResetEvent->ZeEvent));

  if (Event) {
    UR_CALL(createUserEvent(CommandBuffer, Queue, SignalCommandList, *Event));
  }

  UR_CALL(Queue->executeCommandList(SignalCommandList, false, false));

  return UR_RESULT_SUCCESS;
}

UR_APIEXPORT ur_result_t UR_APICALL urCommandBufferRetainCommandExp(
    ur_exp_command_buffer_command_handle_t Command) {
  Command->RefCount.increment();
  return UR_RESULT_SUCCESS;
}

UR_APIEXPORT ur_result_t UR_APICALL urCommandBufferReleaseCommandExp(
    ur_exp_command_buffer_command_handle_t Command) {
  if (!Command->RefCount.decrementAndTest())
    return UR_RESULT_SUCCESS;

  delete Command;
  return UR_RESULT_SUCCESS;
}

namespace {

/**
 * Validates contents of the update command description.
 * @param[in] Command The command which is being updated.
 * @param[in] CommandDesc The update command description.
 * @return UR_RESULT_SUCCESS or an error code on failure
 */
ur_result_t validateCommandDesc(
    ur_exp_command_buffer_command_handle_t Command,
    const ur_exp_command_buffer_update_kernel_launch_desc_t *CommandDesc) {

  auto CommandBuffer = Command->CommandBuffer;
  auto SupportedFeatures =
      Command->CommandBuffer->Device->ZeDeviceMutableCmdListsProperties
          ->mutableCommandFlags;
  logger::debug("Mutable features supported by device {}", SupportedFeatures);

  uint32_t Dim = CommandDesc->newWorkDim;
  if (Dim != 0) {
    // Error if work dim changes
    if (Dim != Command->WorkDim) {
      return UR_RESULT_ERROR_INVALID_OPERATION;
    }

    // Error If Local size and not global size
    if ((CommandDesc->pNewLocalWorkSize != nullptr) &&
        (CommandDesc->pNewGlobalWorkSize == nullptr)) {
      return UR_RESULT_ERROR_INVALID_OPERATION;
    }

    // Error if local size non-nullptr and created with null
    // or if local size nullptr and created with non-null
    const bool IsNewLocalSizeNull = CommandDesc->pNewLocalWorkSize == nullptr;
    const bool IsOriginalLocalSizeNull = !Command->UserDefinedLocalSize;

    if (IsNewLocalSizeNull ^ IsOriginalLocalSizeNull) {
      return UR_RESULT_ERROR_INVALID_OPERATION;
    }
  }

  // Check if new global offset is provided.
  size_t *NewGlobalWorkOffset = CommandDesc->pNewGlobalWorkOffset;
  UR_ASSERT(!NewGlobalWorkOffset ||
                (SupportedFeatures & ZE_MUTABLE_COMMAND_EXP_FLAG_GLOBAL_OFFSET),
            UR_RESULT_ERROR_UNSUPPORTED_FEATURE);
  if (NewGlobalWorkOffset && Dim > 0) {
    if (!CommandBuffer->Context->getPlatform()
             ->ZeDriverGlobalOffsetExtensionFound) {
      logger::error("No global offset extension found on this driver");
      return UR_RESULT_ERROR_INVALID_VALUE;
    }
  }

  // Check if new group size is provided.
  size_t *NewLocalWorkSize = CommandDesc->pNewLocalWorkSize;
  UR_ASSERT(!NewLocalWorkSize ||
                (SupportedFeatures & ZE_MUTABLE_COMMAND_EXP_FLAG_GROUP_SIZE),
            UR_RESULT_ERROR_UNSUPPORTED_FEATURE);

  // Check if new global size is provided and we need to update group count.
  size_t *NewGlobalWorkSize = CommandDesc->pNewGlobalWorkSize;
  UR_ASSERT(!NewGlobalWorkSize ||
                (SupportedFeatures & ZE_MUTABLE_COMMAND_EXP_FLAG_GROUP_COUNT),
            UR_RESULT_ERROR_UNSUPPORTED_FEATURE);
  UR_ASSERT(!(NewGlobalWorkSize && !NewLocalWorkSize) ||
                (SupportedFeatures & ZE_MUTABLE_COMMAND_EXP_FLAG_GROUP_SIZE),
            UR_RESULT_ERROR_UNSUPPORTED_FEATURE);

  UR_ASSERT(
      (!CommandDesc->numNewMemObjArgs && !CommandDesc->numNewPointerArgs &&
       !CommandDesc->numNewValueArgs) ||
          (SupportedFeatures & ZE_MUTABLE_COMMAND_EXP_FLAG_KERNEL_ARGUMENTS),
      UR_RESULT_ERROR_UNSUPPORTED_FEATURE);

  return UR_RESULT_SUCCESS;
}

/**
 * Update the kernel command with the new values.
 * @param[in] Command The command which is being updated.
 * @param[in] CommandDesc The update command description.
 * @return UR_RESULT_SUCCESS or an error code on failure
 */
ur_result_t updateKernelCommand(
    ur_exp_command_buffer_command_handle_t Command,
    const ur_exp_command_buffer_update_kernel_launch_desc_t *CommandDesc) {

  // We need the created descriptors to live till the point when
  // zeCommandListUpdateMutableCommandsExp is called at the end of the
  // function.
  std::vector<std::variant<
      std::unique_ptr<ZeStruct<ze_mutable_kernel_argument_exp_desc_t>>,
      std::unique_ptr<ZeStruct<ze_mutable_global_offset_exp_desc_t>>,
      std::unique_ptr<ZeStruct<ze_mutable_group_size_exp_desc_t>>,
      std::unique_ptr<ZeStruct<ze_mutable_group_count_exp_desc_t>>>>
      Descs;

  const auto CommandBuffer = Command->CommandBuffer;
  const void *NextDesc = nullptr;

  uint32_t Dim = CommandDesc->newWorkDim;
  size_t *NewGlobalWorkOffset = CommandDesc->pNewGlobalWorkOffset;
  size_t *NewLocalWorkSize = CommandDesc->pNewLocalWorkSize;
  size_t *NewGlobalWorkSize = CommandDesc->pNewGlobalWorkSize;

  // Check if a new global offset is provided.
  if (NewGlobalWorkOffset && Dim > 0) {
    auto MutableGroupOffestDesc =
        std::make_unique<ZeStruct<ze_mutable_global_offset_exp_desc_t>>();
    MutableGroupOffestDesc->commandId = Command->CommandId;
    DEBUG_LOG(MutableGroupOffestDesc->commandId);
    MutableGroupOffestDesc->pNext = NextDesc;
    DEBUG_LOG(MutableGroupOffestDesc->pNext);
    MutableGroupOffestDesc->offsetX = NewGlobalWorkOffset[0];
    DEBUG_LOG(MutableGroupOffestDesc->offsetX);
    MutableGroupOffestDesc->offsetY = Dim >= 2 ? NewGlobalWorkOffset[1] : 0;
    DEBUG_LOG(MutableGroupOffestDesc->offsetY);
    MutableGroupOffestDesc->offsetZ = Dim == 3 ? NewGlobalWorkOffset[2] : 0;
    DEBUG_LOG(MutableGroupOffestDesc->offsetZ);

    NextDesc = MutableGroupOffestDesc.get();
    Descs.push_back(std::move(MutableGroupOffestDesc));
  }

  // Check if a new group size is provided.
  if (NewLocalWorkSize && Dim > 0) {
    auto MutableGroupSizeDesc =
        std::make_unique<ZeStruct<ze_mutable_group_size_exp_desc_t>>();
    MutableGroupSizeDesc->commandId = Command->CommandId;
    DEBUG_LOG(MutableGroupSizeDesc->commandId);
    MutableGroupSizeDesc->pNext = NextDesc;
    DEBUG_LOG(MutableGroupSizeDesc->pNext);
    MutableGroupSizeDesc->groupSizeX = NewLocalWorkSize[0];
    DEBUG_LOG(MutableGroupSizeDesc->groupSizeX);
    MutableGroupSizeDesc->groupSizeY = Dim >= 2 ? NewLocalWorkSize[1] : 1;
    DEBUG_LOG(MutableGroupSizeDesc->groupSizeY);
    MutableGroupSizeDesc->groupSizeZ = Dim == 3 ? NewLocalWorkSize[2] : 1;
    DEBUG_LOG(MutableGroupSizeDesc->groupSizeZ);

    NextDesc = MutableGroupSizeDesc.get();
    Descs.push_back(std::move(MutableGroupSizeDesc));
  }

  // Check if a new global size is provided and if we need to update the group
  // count.
  ze_group_count_t ZeThreadGroupDimensions{1, 1, 1};
  if (NewGlobalWorkSize && Dim > 0) {
    // If a new global work size is provided but a new local work size is not
    // then we still need to update local work size based on the size suggested
    // by the driver for the kernel.
    bool UpdateWGSize = NewLocalWorkSize == nullptr;

    uint32_t WG[3];
    UR_CALL(calculateKernelWorkDimensions(
        Command->Kernel->ZeKernel, CommandBuffer->Device,
        ZeThreadGroupDimensions, WG, Dim, NewGlobalWorkSize, NewLocalWorkSize));

    auto MutableGroupCountDesc =
        std::make_unique<ZeStruct<ze_mutable_group_count_exp_desc_t>>();
    MutableGroupCountDesc->commandId = Command->CommandId;
    DEBUG_LOG(MutableGroupCountDesc->commandId);
    MutableGroupCountDesc->pNext = NextDesc;
    DEBUG_LOG(MutableGroupCountDesc->pNext);
    MutableGroupCountDesc->pGroupCount = &ZeThreadGroupDimensions;
    DEBUG_LOG(MutableGroupCountDesc->pGroupCount->groupCountX);
    DEBUG_LOG(MutableGroupCountDesc->pGroupCount->groupCountY);
    DEBUG_LOG(MutableGroupCountDesc->pGroupCount->groupCountZ);

    NextDesc = MutableGroupCountDesc.get();
    Descs.push_back(std::move(MutableGroupCountDesc));

    if (UpdateWGSize) {
      auto MutableGroupSizeDesc =
          std::make_unique<ZeStruct<ze_mutable_group_size_exp_desc_t>>();
      MutableGroupSizeDesc->commandId = Command->CommandId;
      DEBUG_LOG(MutableGroupSizeDesc->commandId);
      MutableGroupSizeDesc->pNext = NextDesc;
      DEBUG_LOG(MutableGroupSizeDesc->pNext);
      MutableGroupSizeDesc->groupSizeX = WG[0];
      DEBUG_LOG(MutableGroupSizeDesc->groupSizeX);
      MutableGroupSizeDesc->groupSizeY = WG[1];
      DEBUG_LOG(MutableGroupSizeDesc->groupSizeY);
      MutableGroupSizeDesc->groupSizeZ = WG[2];
      DEBUG_LOG(MutableGroupSizeDesc->groupSizeZ);

      NextDesc = MutableGroupSizeDesc.get();
      Descs.push_back(std::move(MutableGroupSizeDesc));
    }
  }

  // Check if new memory object arguments are provided.
  for (uint32_t NewMemObjArgNum = CommandDesc->numNewMemObjArgs;
       NewMemObjArgNum-- > 0;) {
    ur_exp_command_buffer_update_memobj_arg_desc_t NewMemObjArgDesc =
        CommandDesc->pNewMemObjArgList[NewMemObjArgNum];
    const ur_kernel_arg_mem_obj_properties_t *Properties =
        NewMemObjArgDesc.pProperties;
    ur_mem_handle_t_::access_mode_t UrAccessMode = ur_mem_handle_t_::read_write;
    if (Properties) {
      switch (Properties->memoryAccess) {
      case UR_MEM_FLAG_READ_WRITE:
        UrAccessMode = ur_mem_handle_t_::read_write;
        break;
      case UR_MEM_FLAG_WRITE_ONLY:
        UrAccessMode = ur_mem_handle_t_::write_only;
        break;
      case UR_MEM_FLAG_READ_ONLY:
        UrAccessMode = ur_mem_handle_t_::read_only;
        break;
      default:
        return UR_RESULT_ERROR_INVALID_ARGUMENT;
      }
    }

    ur_mem_handle_t NewMemObjArg = NewMemObjArgDesc.hNewMemObjArg;
    // The NewMemObjArg may be a NULL pointer in which case a NULL value is used
    // for the kernel argument declared as a pointer to global or constant
    // memory.
    char **ZeHandlePtr = nullptr;
    if (NewMemObjArg) {
      UR_CALL(NewMemObjArg->getZeHandlePtr(ZeHandlePtr, UrAccessMode,
                                           CommandBuffer->Device));
    }

    auto ZeMutableArgDesc =
        std::make_unique<ZeStruct<ze_mutable_kernel_argument_exp_desc_t>>();
    ZeMutableArgDesc->commandId = Command->CommandId;
    DEBUG_LOG(ZeMutableArgDesc->commandId);
    ZeMutableArgDesc->pNext = NextDesc;
    DEBUG_LOG(ZeMutableArgDesc->pNext);
    ZeMutableArgDesc->argIndex = NewMemObjArgDesc.argIndex;
    DEBUG_LOG(ZeMutableArgDesc->argIndex);
    ZeMutableArgDesc->argSize = sizeof(void *);
    DEBUG_LOG(ZeMutableArgDesc->argSize);
    ZeMutableArgDesc->pArgValue = ZeHandlePtr;
    DEBUG_LOG(ZeMutableArgDesc->pArgValue);

    NextDesc = ZeMutableArgDesc.get();
    Descs.push_back(std::move(ZeMutableArgDesc));
  }

  // Check if there are new pointer arguments.
  for (uint32_t NewPointerArgNum = CommandDesc->numNewPointerArgs;
       NewPointerArgNum-- > 0;) {
    ur_exp_command_buffer_update_pointer_arg_desc_t NewPointerArgDesc =
        CommandDesc->pNewPointerArgList[NewPointerArgNum];

    auto ZeMutableArgDesc =
        std::make_unique<ZeStruct<ze_mutable_kernel_argument_exp_desc_t>>();
    ZeMutableArgDesc->commandId = Command->CommandId;
    DEBUG_LOG(ZeMutableArgDesc->commandId);
    ZeMutableArgDesc->pNext = NextDesc;
    DEBUG_LOG(ZeMutableArgDesc->pNext);
    ZeMutableArgDesc->argIndex = NewPointerArgDesc.argIndex;
    DEBUG_LOG(ZeMutableArgDesc->argIndex);
    ZeMutableArgDesc->argSize = sizeof(void *);
    DEBUG_LOG(ZeMutableArgDesc->argSize);
    ZeMutableArgDesc->pArgValue = NewPointerArgDesc.pNewPointerArg;
    DEBUG_LOG(ZeMutableArgDesc->pArgValue);

    NextDesc = ZeMutableArgDesc.get();
    Descs.push_back(std::move(ZeMutableArgDesc));
  }

  // Check if there are new value arguments.
  for (uint32_t NewValueArgNum = CommandDesc->numNewValueArgs;
       NewValueArgNum-- > 0;) {
    ur_exp_command_buffer_update_value_arg_desc_t NewValueArgDesc =
        CommandDesc->pNewValueArgList[NewValueArgNum];

    auto ZeMutableArgDesc =
        std::make_unique<ZeStruct<ze_mutable_kernel_argument_exp_desc_t>>();
    ZeMutableArgDesc->commandId = Command->CommandId;
    DEBUG_LOG(ZeMutableArgDesc->commandId);
    ZeMutableArgDesc->pNext = NextDesc;
    DEBUG_LOG(ZeMutableArgDesc->pNext);
    ZeMutableArgDesc->argIndex = NewValueArgDesc.argIndex;
    DEBUG_LOG(ZeMutableArgDesc->argIndex);
    ZeMutableArgDesc->argSize = NewValueArgDesc.argSize;
    DEBUG_LOG(ZeMutableArgDesc->argSize);
    // OpenCL: "the arg_value pointer can be NULL or point to a NULL value
    // in which case a NULL value will be used as the value for the argument
    // declared as a pointer to global or constant memory in the kernel"
    //
    // We don't know the type of the argument but it seems that the only time
    // SYCL RT would send a pointer to NULL in 'arg_value' is when the argument
    // is a NULL pointer. Treat a pointer to NULL in 'arg_value' as a NULL.
    const void *ArgValuePtr = NewValueArgDesc.pNewValueArg;
    if (NewValueArgDesc.argSize == sizeof(void *) && ArgValuePtr &&
        *(void **)(const_cast<void *>(ArgValuePtr)) == nullptr) {
      ArgValuePtr = nullptr;
    }
    ZeMutableArgDesc->pArgValue = ArgValuePtr;
    DEBUG_LOG(ZeMutableArgDesc->pArgValue);

    NextDesc = ZeMutableArgDesc.get();
    Descs.push_back(std::move(ZeMutableArgDesc));
  }

  ZeStruct<ze_mutable_commands_exp_desc_t> MutableCommandDesc;
  MutableCommandDesc.pNext = NextDesc;
  MutableCommandDesc.flags = 0;

  auto Platform = CommandBuffer->Context->getPlatform();
  ZE2UR_CALL(
      Platform->ZeMutableCmdListExt.zexCommandListUpdateMutableCommandsExp,
      (CommandBuffer->ZeComputeCommandListTranslated, &MutableCommandDesc));

  return UR_RESULT_SUCCESS;
}
} // namespace

UR_APIEXPORT ur_result_t UR_APICALL urCommandBufferUpdateKernelLaunchExp(
    ur_exp_command_buffer_command_handle_t Command,
    const ur_exp_command_buffer_update_kernel_launch_desc_t *CommandDesc) {
  UR_ASSERT(Command->Kernel, UR_RESULT_ERROR_INVALID_NULL_HANDLE);
  UR_ASSERT(CommandDesc->newWorkDim <= 3,
            UR_RESULT_ERROR_INVALID_WORK_DIMENSION);

  // Lock command, kernel and command buffer for update.
  std::scoped_lock<ur_shared_mutex, ur_shared_mutex, ur_shared_mutex> Guard(
      Command->Mutex, Command->CommandBuffer->Mutex, Command->Kernel->Mutex);

  UR_ASSERT(Command->CommandBuffer->IsUpdatable,
            UR_RESULT_ERROR_INVALID_OPERATION);
  UR_ASSERT(Command->CommandBuffer->IsFinalized,
            UR_RESULT_ERROR_INVALID_OPERATION);

  UR_CALL(validateCommandDesc(Command, CommandDesc));

  // We must synchronize mutable command list execution before mutating.
  if (ze_fence_handle_t &ZeFence = Command->CommandBuffer->ZeActiveFence) {
    ZE2UR_CALL(zeFenceHostSynchronize, (ZeFence, UINT64_MAX));
  }

  UR_CALL(updateKernelCommand(Command, CommandDesc));

  ZE2UR_CALL(zeCommandListClose,
             (Command->CommandBuffer->ZeComputeCommandList));

  return UR_RESULT_SUCCESS;
}

UR_APIEXPORT ur_result_t UR_APICALL urCommandBufferGetInfoExp(
    ur_exp_command_buffer_handle_t hCommandBuffer,
    ur_exp_command_buffer_info_t propName, size_t propSize, void *pPropValue,
    size_t *pPropSizeRet) {
  UrReturnHelper ReturnValue(propSize, pPropValue, pPropSizeRet);

  switch (propName) {
  case UR_EXP_COMMAND_BUFFER_INFO_REFERENCE_COUNT:
    return ReturnValue(uint32_t{hCommandBuffer->RefCount.load()});
  default:
    assert(!"Command-buffer info request not implemented");
  }

  return UR_RESULT_ERROR_INVALID_ENUMERATION;
}

UR_APIEXPORT ur_result_t UR_APICALL urCommandBufferCommandGetInfoExp(
    ur_exp_command_buffer_command_handle_t Command,
    ur_exp_command_buffer_command_info_t PropName, size_t PropSize,
    void *PropValue, size_t *PropSizeRet) {
  UrReturnHelper ReturnValue(PropSize, PropValue, PropSizeRet);

  switch (PropName) {
  case UR_EXP_COMMAND_BUFFER_COMMAND_INFO_REFERENCE_COUNT:
    return ReturnValue(uint32_t{Command->RefCount.load()});
  default:
    assert(!"Command-buffer command info request not implemented");
  }

  return UR_RESULT_ERROR_INVALID_ENUMERATION;
}
