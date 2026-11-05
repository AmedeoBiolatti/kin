# Python garden: a platformer played in Python (docs/python_scripting.md).
add_executable(python_garden main.cpp)
target_link_libraries(python_garden PRIVATE kin::python_scripts)
if(KIN_ENABLE_PHYSICS)
    target_link_libraries(python_garden PRIVATE kin::physics_ecs)
endif()
kin_game(python_garden TITLE "Python garden" CONTENT "${CMAKE_CURRENT_SOURCE_DIR}/content")
if(BUILD_TESTING AND KIN_BUILD_TESTS)
    # Ten seconds and more of the gardener playing itself: no script raises,
    # something is picked.
    add_test(NAME kin-python-garden COMMAND python_garden --headless --frames=900 --seed=1 --mute)
    set_tests_properties(kin-python-garden PROPERTIES TIMEOUT 60 LABELS "component:PYTHON;integration")
endif()
