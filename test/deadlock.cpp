
#include <sycl/sycl.hpp>

template <typename T>
using global_atomic_ref =
    sycl::atomic_ref<T, sycl::memory_order::relaxed, sycl::memory_scope::system,
                     sycl::access::address_space::global_space>;

int main() {
    auto devs = sycl::device::get_devices(sycl::info::device_type::gpu);
    if (devs.size() < 2) {
            std::cout << "Requires 2 or more devices" << std::endl;
            return -1;
    }

    sycl::queue deviceQueue1(devs[0]);
    sycl::queue deviceQueue2(devs[1]);

    auto ptr1 = sycl::malloc_shared<uint64_t>(1, deviceQueue1);
    auto ptr2 = sycl::malloc_shared<uint64_t>(1, deviceQueue1);

    *ptr2 = 0;

    auto e1 = deviceQueue1.submit([&](sycl::handler &cgh) {
        cgh.single_task<class store>([=]() {
           *ptr1 = 1;
        });
    });

    auto e2 = deviceQueue1.submit([&](sycl::handler &cgh) {
        cgh.single_task<class atomic_wait>([=]() {
            global_atomic_ref<uint64_t> atomic(*ptr2);
            while (atomic.load() == 0) {
            }
        });
    });

    e1.wait();
    sycl::free(ptr1, deviceQueue1);

    auto signal = deviceQueue2.submit([&](sycl::handler &cgh) {
        cgh.single_task<class atomic_store>([=]() {
            global_atomic_ref<uint64_t> atomic(*ptr2);
            atomic.store(1);
        });
    });

    e2.wait();
    signal.wait();

    return 0;
}
