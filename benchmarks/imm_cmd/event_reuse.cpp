#include <atomic>
#include <iostream>
#include <fstream>
#include <string>
#include <sstream>
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

std::string readFileIntoString(const std::string& path) {
    std::ifstream input_file(path);
    if (!input_file.is_open()) {
        std::cerr << "Could not open the file - '" << path << "'" << std::endl;
        return "";
    }
    std::stringstream buffer;
    buffer << input_file.rdbuf();
    return buffer.str();
}

int main() {
    UR ur;
    UR_ASSERT(ur.init());

    std::cout << ur.backend() << std::endl;

    auto kernel_content = readFileIntoString("../test/conformance/device_binaries/bar/sycl_spir64.spv");
    auto kernel_name = "_ZTSZZ4mainENKUlRN4sycl3_V17handlerEE_clES2_E3Bar";

    ur_program_handle_t program;
    UR_ASSERT(urProgramCreateWithIL(ur.context, kernel_content.data(), kernel_content.size(), nullptr,
                          &program));
    UR_ASSERT(urProgramBuild(ur.context, program, nullptr));
    ur_kernel_handle_t kernel;
    UR_ASSERT(urKernelCreate(program, kernel_name, &kernel));

    const uint32_t nDim = 3;
    const size_t gWorkOffset[] = {0, 0, 0};
    const size_t gWorkSize[] = {128, 128, 128};

    ur_queue_handle_t q;
    ur_queue_properties_t props;
    props.flags = UR_QUEUE_FLAG_SUBMISSION_IMMEDIATE |
                  UR_QUEUE_FLAG_OUT_OF_ORDER_EXEC_MODE_ENABLE;
    props.pNext = nullptr;
    props.stype = UR_STRUCTURE_TYPE_QUEUE_PROPERTIES;
    ur.queue_create(ur.devices[0], props, q);

    for (int i = 0; i < 1000; ++i) {
        urEnqueueKernelLaunch(q, kernel, nDim, gWorkOffset, gWorkSize, nullptr,
                            0, nullptr, nullptr);
    }

    Bench bench;

    bench.epochs(10000).minEpochIterations(1).run("enqueue", [&] {
        urEnqueueKernelLaunch(q, kernel, nDim, gWorkOffset, gWorkSize, nullptr,
                            0, nullptr, nullptr);
    });

    ur.teardown();

    gen("html", ankerl::nanobench::templates::htmlBoxplot(), bench);

    return 0;
}
