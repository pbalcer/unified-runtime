#include "ur_api.h"
#include <string>
#include <vector>
#include <assert.h>

#define UR_TRY(f)\
do {\
if (auto result = (f); result != UR_RESULT_SUCCESS) return result;\
} while (0)

#define UR_ASSERT(f)\
do {\
auto result = (f);\
assert(result == UR_RESULT_SUCCESS);\
} while (0)


struct UR {
    ur_result_t init() {
        UR_TRY(urLoaderInit(0, nullptr));

        uint32_t nadapters;
        UR_TRY(urAdapterGet(1, &adapter, &nadapters));
        if (nadapters != 1) {
            teardown();
            return UR_RESULT_ERROR_UNINITIALIZED;
        }

        uint32_t nplatforms;
        UR_TRY(urPlatformGet(&adapter, 1, 1, &platform, &nplatforms));
        if (nplatforms != 1) {
            teardown();
            return UR_RESULT_ERROR_UNINITIALIZED;
        }

        uint32_t ndevices = 0;
        UR_TRY(urDeviceGet(platform, UR_DEVICE_TYPE_GPU, 0, nullptr, &ndevices));
        if (ndevices == 0) {
            teardown();
            return UR_RESULT_ERROR_UNINITIALIZED;
        }

        devices.resize(ndevices);
        UR_TRY(urDeviceGet(platform, UR_DEVICE_TYPE_GPU, ndevices, devices.data(), nullptr));

        UR_TRY(urContextCreate(devices.size(), devices.data(), nullptr, &context));

        return UR_RESULT_SUCCESS;
    }

    ur_result_t queue_create(ur_device_handle_t device, ur_queue_properties_t props, ur_queue_handle_t &queue) {
        return urQueueCreate(context, device, &props, &queue);
    }

    ur_result_t queue_delete(ur_queue_handle_t queue) {
        return urQueueRelease(queue);
    }

    void teardown() {
        urContextRelease(context);
        for (auto d : devices) {
            urDeviceRelease(d);
        }
        if (adapter != nullptr) {
            urAdapterRelease(adapter);
        }
        urLoaderTearDown();
    }

    ur_platform_backend_t backend() {
        ur_platform_backend_t backend;
        urPlatformGetInfo(platform, UR_PLATFORM_INFO_BACKEND, sizeof(backend), &backend, nullptr);

        return backend;
    }

    ur_context_handle_t context;
    ur_adapter_handle_t adapter;
    ur_platform_handle_t platform;
    std::vector<ur_device_handle_t> devices;
};
