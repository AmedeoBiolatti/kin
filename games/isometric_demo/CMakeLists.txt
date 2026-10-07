add_executable(isometric_demo
    src/iso_draw.cpp
    src/main.cpp
)

target_link_libraries(isometric_demo PRIVATE
    kin::engine
)

# The water shader, built into the demo. Without glslc it is not built, and the
# water is drawn plainly.
kin_compile_glsl(_iso_spv OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders" SOURCES shaders/water.frag.glsl)
kin_embed_files(isometric_demo NAME isometric_shader FILES ${_iso_spv})

kin_game(isometric_demo TITLE "Isometric Demo")
