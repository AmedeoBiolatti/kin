add_executable(ecs_graph_demo
    src/main.cpp
)

target_link_libraries(ecs_graph_demo PRIVATE
    kin::engine
)

kin_game(ecs_graph_demo TITLE "ECS Graph Demo")

target_compile_options(ecs_graph_demo PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
)

add_test(NAME ecs-graph-demo
    COMMAND ecs_graph_demo --headless --frames=80 --report=-
)
