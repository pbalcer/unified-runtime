add_library(benchmark_common INTERFACE fixtures.hpp)
target_link_libraries(benchmark_common INTERFACE
    ${PROJECT_NAME}::loader
    ${CMAKE_DL_LIBS}
    ${PROJECT_NAME}::headers
)
target_include_directories(benchmark_common INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
)