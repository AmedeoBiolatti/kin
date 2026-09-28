add_executable(ecs_parallel_bench
    src/main.cpp
)

target_link_libraries(ecs_parallel_bench PRIVATE
    kin::engine
)

target_compile_options(ecs_parallel_bench PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
)

add_test(NAME ecs-parallel-bench
    COMMAND ecs_parallel_bench --frames=2 --warmup=1 --entities=256 --iterations=512 --min-speedup=0.0
)
