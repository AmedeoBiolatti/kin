add_executable(kin_2d_workflow main.cpp)
target_link_libraries(kin_2d_workflow PRIVATE kin::scripting_scenes)
kin_game(kin_2d_workflow TITLE "Kin 2D Workshop"
    CONTENT "${CMAKE_CURRENT_SOURCE_DIR}/content"
    IMPORTS "${CMAKE_CURRENT_SOURCE_DIR}/art/imports.json")
if(BUILD_TESTING AND KIN_BUILD_TESTS)
    add_test(NAME kin-workflow-demo COMMAND kin_2d_workflow --self-test)
    set_tests_properties(kin-workflow-demo PROPERTIES TIMEOUT 60 LABELS "component:SCRIPTING;integration")
endif()
