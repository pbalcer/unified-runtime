#include <atomic>
#include <fstream>
#include <iostream>
#include <nanobench.h>

#include "fixtures.hpp"
#include "ur_api.h"
#include "ur_print.hpp"

using namespace ankerl::nanobench;

void gen(std::string const &typeName, char const *mustacheTemplate,
         Bench const &bench) {

    std::ofstream templateOut("template." + typeName);

    templateOut << mustacheTemplate;

    std::ofstream renderOut("chart." + typeName);

    render(mustacheTemplate, bench, renderOut);
}

int main() {
    UR ur;
    UR_ASSERT(ur.init());

    std::cout << ur.backend() << std::endl;

    constexpr size_t buf_size = 4096;
    ur_mem_handle_t buffer;
    UR_ASSERT(urMemBufferCreate(ur.context, UR_MEM_FLAG_READ_WRITE, buf_size,
                                nullptr, &buffer));

    ur_queue_handle_t q;
    ur_queue_properties_t props;
    props.flags = UR_QUEUE_FLAG_SUBMISSION_IMMEDIATE |
                  UR_QUEUE_FLAG_OUT_OF_ORDER_EXEC_MODE_ENABLE;
    props.pNext = nullptr;
    props.stype = UR_STRUCTURE_TYPE_QUEUE_PROPERTIES;
    ur.queue_create(ur.devices[0], props, q);

    char src[buf_size];
    memset(src, 0xc, buf_size);

    Bench bench;

    bench.epochs(10000).epochIterations(1).run("enqueue", [&] {
        urEnqueueMemBufferWrite(q, buffer, false, 0, buf_size, src, 0, nullptr,
                                nullptr);
    });

    urMemRelease(buffer);
    ur.teardown();

    gen("html", ankerl::nanobench::templates::htmlBoxplot(), bench);

    return 0;
}
