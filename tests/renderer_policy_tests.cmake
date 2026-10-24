function(reject choice expected)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env
        "SDL_VIDEO_DRIVER=dummy" "KIN_RENDER_BACKEND=${choice}" "${EXECUTABLE}"
        RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 20)
    if(result STREQUAL "0" OR NOT "${out}${err}" MATCHES "${expected}")
        message(FATAL_ERROR "Renderer policy '${choice}' was not rejected as expected: ${result}\n${out}${err}")
    endif()
endfunction()

reject(typo "KIN_RENDER_BACKEND must be")
if(NOT RENDERER STREQUAL "webgl2")
    reject(webgl2 "requires KIN_RENDERER=webgl2")
endif()
if(RENDERER STREQUAL "compact")
    reject(sdl "conflicts with KIN_RENDERER=compact")
    reject(gpu "conflicts with KIN_RENDERER=compact")
else()
    reject(compact "requires KIN_RENDERER=compact")
endif()
if(RENDERER STREQUAL "software")
    reject(gpu "conflicts with KIN_RENDERER=software")
elseif(RENDERER STREQUAL "gpu")
    reject(sdl "conflicts with KIN_RENDERER=gpu")
    # SDL's dummy driver cannot host a GPU swapchain. A fixed GPU build must
    # fail device creation here, never silently start a software renderer.
    reject("" "SDL_CreateGPUDevice|SDL_ClaimWindowForGPUDevice|swapchain unavailable")
endif()
