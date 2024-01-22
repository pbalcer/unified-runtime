
add_executable(event_reuse event_reuse.cpp)
target_link_libraries(event_reuse PRIVATE
    nanobench
    benchmark_common
)
