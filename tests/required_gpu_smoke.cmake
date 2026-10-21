# Exercise both contracts with a deliberately unavailable renderer or asset.
# The outer CI may require a GPU; each child gets an explicit policy.
if(MODE STREQUAL "sponza")
    set(args --model "${CMAKE_CURRENT_BINARY_DIR}/intentionally-absent-model.gltf")
endif()
foreach(required 0 1)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env
        KIN_RENDER_BACKEND=sdl SDL_VIDEO_DRIVER=dummy SDL_VIDEODRIVER=dummy
        SDL_AUDIO_DRIVER=dummy SDL_AUDIODRIVER=dummy KIN_REQUIRE_GPU_TESTS=${required}
        "${PROGRAM}" --headless --frames 1 --skip-if-unavailable ${args}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 30)
    if(required EQUAL 0)
        set(expected 77)
    else()
        set(expected 1)
    endif()
    if(NOT result STREQUAL "${expected}")
        message(FATAL_ERROR "${MODE} require=${required}: expected ${expected}, got ${result}\n${output}\n${error}")
    endif()
endforeach()
