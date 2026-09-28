add_executable(isometric_demo
    src/iso_draw.cpp
    src/main.cpp
)

target_link_libraries(isometric_demo PRIVATE
    kin::engine
)

find_program(KIN_GLSLC NAMES glslc HINTS "$ENV{VULKAN_SDK}/bin" "$ENV{VULKAN_SDK}/Bin")
set(_iso_shader_dir "${CMAKE_CURRENT_BINARY_DIR}/shaders")
target_compile_definitions(isometric_demo PRIVATE KIN_ISOMETRIC_DEMO_SHADER_DIR="${_iso_shader_dir}")

if(KIN_GLSLC)
    file(MAKE_DIRECTORY "${_iso_shader_dir}")
    add_custom_command(
        OUTPUT "${_iso_shader_dir}/water.frag.spv"
        COMMAND "${KIN_GLSLC}" -fshader-stage=fragment
                -o "${_iso_shader_dir}/water.frag.spv"
                "${CMAKE_CURRENT_SOURCE_DIR}/shaders/water.frag.glsl"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/water.frag.glsl"
        COMMENT "glslc: water.frag.glsl -> water.frag.spv"
        VERBATIM)
    add_custom_target(isometric_demo_shaders DEPENDS "${_iso_shader_dir}/water.frag.spv")
    add_dependencies(isometric_demo isometric_demo_shaders)
else()
    message(STATUS "isometric_demo: glslc not found; water shader not compiled")
endif()

kin_enable_release_optimizations(isometric_demo)
