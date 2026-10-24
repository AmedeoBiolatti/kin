if(KIN_BUILD_WEBGL2_FIXTURE)
    add_executable(webgl2_3d_fixture main.cpp render_checks.cpp)
    target_link_libraries(webgl2_3d_fixture PRIVATE kin::engine)
    kin_game(webgl2_3d_fixture TITLE "Kin WebGL2: 2D and 3D" NO_CONTENT)
endif()

if(KIN_BUILD_WEB_SUMMIT)
    if(NOT (KIN_ENABLE_PHYSICS3D AND KIN_ENABLE_AUDIO AND KIN_ENABLE_IMAGE_IO AND KIN_ENABLE_TTF))
        message(FATAL_ERROR "Browser Summit requires 3D physics, audio, image IO and TTF")
    endif()
    set(summit "${PROJECT_SOURCE_DIR}/examples/summit")
    add_library(kin_web_summit_sim STATIC
        "${summit}/level.cpp" "${summit}/game.cpp" "${summit}/script.cpp" "${summit}/input_map.cpp"
        "${summit}/camera.cpp" "${summit}/rock.cpp" "${summit}/climber.cpp" "${summit}/settings.cpp"
        "${summit}/environment.cpp" "${summit}/hud.cpp" "${summit}/audio.cpp" "${summit}/expedition.cpp" "${summit}/props.cpp")
    target_include_directories(kin_web_summit_sim PUBLIC "${PROJECT_SOURCE_DIR}/examples")
    target_link_libraries(kin_web_summit_sim PUBLIC kin::engine)
    kin_enable_release_optimizations(kin_web_summit_sim)
    add_executable(summit_web "${summit}/main.cpp" "${summit}/view.cpp" "${summit}/hud_draw.cpp"
        "${summit}/external_assets.cpp" "${summit}/benchmark.cpp")
    target_link_libraries(summit_web PRIVATE kin_web_summit_sim)
    target_link_options(summit_web PRIVATE -sSTACK_SIZE=1048576)
    set(font "${PROJECT_SOURCE_DIR}/examples/content/lang/fonts/latin.ttf")
    target_link_options(summit_web PRIVATE "SHELL:--preload-file \"${font}@/summit/fonts/latin.ttf\"")
    set_property(TARGET summit_web APPEND PROPERTY LINK_DEPENDS "${font}")
    set_property(TARGET summit_web PROPERTY KIN_WEB_EXTRA_LICENSES "${PROJECT_SOURCE_DIR}/examples/content/lang/fonts/core-OFL.txt")
    kin_game(summit_web NAME summit TITLE "Summit" CONTENT "${summit}/content")

    # Run the existing native gameplay suites in the browser against the same Jolt build.
    add_executable(summit_web_simulation simulation_driver.cpp
        "${summit}/summit_tests.cpp" "${summit}/expedition_tests.cpp")
    target_compile_definitions(summit_web_simulation PRIVATE KIN_WEB_TEST_DRIVER=1)
    target_link_libraries(summit_web_simulation PRIVATE kin_web_summit_sim)
    target_link_options(summit_web_simulation PRIVATE -sSTACK_SIZE=1048576)
    kin_game(summit_web_simulation TITLE "Summit browser simulation checks" NO_CONTENT)
endif()
