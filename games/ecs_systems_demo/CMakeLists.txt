add_executable(ecs_systems_demo
    src/main.cpp
)

target_link_libraries(ecs_systems_demo PRIVATE
    kin::engine
)

kin_game(ecs_systems_demo TITLE "ECS Systems Demo")

target_compile_options(ecs_systems_demo PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
)
