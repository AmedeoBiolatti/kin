add_executable(kin_2d_workflow main.cpp)
target_link_libraries(kin_2d_workflow PRIVATE kin::scripting_scenes)
kin_game(kin_2d_workflow TITLE "Kin 2D Workshop"
    CONTENT "${CMAKE_CURRENT_SOURCE_DIR}/content"
    IMPORTS "${CMAKE_CURRENT_SOURCE_DIR}/art/imports.json")
if(BUILD_TESTING AND KIN_BUILD_TESTS)
    add_test(NAME kin-workflow-demo COMMAND kin_2d_workflow --self-test)
    set_tests_properties(kin-workflow-demo PROPERTIES TIMEOUT 60 LABELS "component:SCRIPTING;integration")
    if(KIN_ENABLE_SCENE_SERVER AND TARGET kin_scenario)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        get_target_property(workflow_content kin_2d_workflow KIN_IMPORTED_CONTENT_DIR)
        add_test(NAME kin-workflow-automation
            COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/automation_integration.py"
                --binary $<TARGET_FILE:kin_2d_workflow> --compiler $<TARGET_FILE:kin_scenario>
                --content "${workflow_content}" --output "${CMAKE_CURRENT_BINARY_DIR}/automation")
        set_tests_properties(kin-workflow-automation PROPERTIES TIMEOUT 180 LABELS "component:SCENE_SERVER;integration")
        add_test(NAME kin-workflow-development COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/development_integration.py"
            --binary $<TARGET_FILE:kin_2d_workflow> --content "${workflow_content}" --output "${CMAKE_CURRENT_BINARY_DIR}/development")
        set_tests_properties(kin-workflow-development PROPERTIES TIMEOUT 180 LABELS "component:SCENE_SERVER;integration")
    endif()
endif()
