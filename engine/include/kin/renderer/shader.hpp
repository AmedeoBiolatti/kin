#pragma once

#include <kin/core/types.hpp>
#include <kin/renderer/color.hpp>

#include <array>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace kin {

// Custom 2D material shaders (e.g. a lava/glass panel). Implemented on the SDL
// "gpu" render driver (SDL 3.4 render-GPU-state); capabilities().materials_2d is
// true only there. On any other backend draw_shader_surface() is a no-op and the
// UI layer supplies a fallback fill, so callers degrade gracefully.

struct ShaderHandle {
    u64 value = 0;

    explicit operator bool() const { return value != 0; }
    friend constexpr bool operator==(ShaderHandle, ShaderHandle) = default;
};

// Most textures one material shader can sample (fragment sampler slots 0..15),
// the per-stage limit SDL_GPU guarantees on every backend.
inline constexpr u32 MaxShaderSamplers = 16;

// A single precompiled fragment-shader binary in one GPU format. The backend
// picks the blob matching the device's supported format (SDL_GetGPUShaderFormats).
struct ShaderBlob {
    const u8* code = nullptr;
    u32 size = 0;

    bool valid() const { return code != nullptr && size > 0; }
};

// Describes a custom fragment-shader material to create_shader(). Author the
// shader once in HLSL matching SDL's 2D-GPU contract — the drawn texture binds to
// `Texture2D u_texture : register(t0, space2)` / `SamplerState : register(s0, space2)`,
// the fragment input is `float4 v_color : COLOR0; float2 v_uv : TEXCOORD0`, the
// output is `SV_Target`, and an optional uniform block is `cbuffer : register(b0, space3)`
// (fed from ShaderParams via slot 0). Provide whichever precompiled formats you have.
// draw_shader_geometry() feeds each vertex's ShaderVertex::custom to the fragment
// stage at `layout(location = 2) in vec4` (TEXCOORD1 in HLSL).
// The textures passed to draw_shader_surface() bind in order at sampler slots 0, 1,
// 2, ... (`register(tN, space2)`, or `layout(set = 2, binding = N)` in GLSL). Slots the
// shader declares but the draw leaves out are bound to a 1x1 white texture.
struct ShaderDesc {
    ShaderBlob spirv;            // Vulkan
    ShaderBlob dxil;             // D3D12
    ShaderBlob dxbc;             // D3D11
    ShaderBlob msl;              // Metal
    u32 num_samplers = 1;        // sampler slots the shader declares, 1..MaxShaderSamplers
    u32 num_uniform_buffers = 0; // fragment uniform buffers (ShaderParams -> slot 0)
    u32 num_storage_buffers = 0; // fragment storage buffers (DataBuffer), after the samplers in set 2
    const char* entrypoint = "main";
};

// Most floats a ShaderParams block may hold: 16 KiB, the uniform-block size every
// Vulkan device guarantees.
inline constexpr u32 MaxShaderUniformFloats = 4096;

// Uniform block fed to the shader's `cbuffer : register(b0, space3)` (GLSL:
// `layout(set = 3, binding = 0) uniform`). Layout is up to the shader author.
// 16 floats (four vec4s) by default; resize for more, up to
// MaxShaderUniformFloats. Declare arrays as vec4s: std140 pads each element of a
// float array to 16 bytes.
struct ShaderLayout;

struct ShaderParams {
    std::vector<f32> uniforms = std::vector<f32>(16, 0.0f);
    // The shader's layout (Renderer2D::shader_params sets it), for set() by name.
    std::shared_ptr<const ShaderLayout> layout;

    // Writes `values` at the uniform block member `name` (as declared in the
    // shader). False without a layout, for an unknown name, or past the member.
    bool set(std::string_view name, std::span<const f32> values);
    bool set(std::string_view name, f32 value) { return set(name, std::span<const f32>{&value, 1}); }
};

// A vertex of draw_shader_geometry(). The fragment shader gets `color` and `uv`
// as for a shader surface, and `custom` as `layout(location = 2) in vec4`: per
// vertex data, so one draw can carry many shapes with their own parameters (an
// index into a data texture, a height, a strength).
struct ShaderVertex {
    Vec2f position{}; // in the same coordinates as other draws
    Vec2f uv{};
    Color color = Color::rgb(255, 255, 255);
    std::array<f32, 4> custom{};
};

// Engine-shipped fragment shaders, compiled to SPIR-V in KIN_GPU_SHADER_DIR and loaded
// on demand via Renderer2D::builtin_shader(). Post-fx are single-input except BloomCombine
// (samples blurred + original). Transition shaders take a progress uniform; Crossfade is
// 2-input (A + B). Returns a null handle on backends without material support (degrade).
enum class BuiltinShader {
    // --- post-processing ---
    Vignette,
    ColorGrade,
    Scanline,
    Chroma,
    BloomBright,
    BloomBlur,
    BloomCombine, // 2-input: blurred bright-pass + original scene
    // --- scene transitions ---
    TransitionDissolve,
    TransitionPixelate,
    TransitionWipe,
    TransitionIris,
    TransitionCrossfade, // 2-input: outgoing + incoming
};

} // namespace kin
