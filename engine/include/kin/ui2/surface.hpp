#pragma once

#include <kin/core/types.hpp>
#include <kin/renderer/color.hpp>
#include <kin/renderer/gradient.hpp>
#include <kin/renderer/shader.hpp>
#include <kin/ui2/image.hpp>

namespace kin::ui2 {

enum class BorderMode {
    None,
    Inside,
};

// How a surface's interior is filled. Glass/Shader gracefully degrade when the
// renderer lacks the capability (see Context::surface()).
enum class SurfaceFill {
    Solid,    // flat `fill`
    Gradient, // two-color `gradient`
    Glass,    // frosted backdrop (degrades to a translucent `glass_tint`)
    Shader,   // custom material (degrades to `fill`)
};

// 1px inset light/dark edge lines (top-left light, bottom-right dark).
struct BevelStyle {
    Color light = colors::transparent;
    Color dark = colors::transparent;
    bool enabled = false;
};

// Short dark gradient at the top inside edge — makes inputs read as recessed.
struct InnerShadowStyle {
    Color color = colors::transparent;
    f32 depth = 0.0f;
    bool enabled = false;
};

struct ShadowStyle {
    Color color = colors::transparent;
    Vec2f offset{};
    f32 spread = 0.0f;
    f32 radius = 0.0f;
    i32 layers = 1;
    bool enabled = false;
};

struct SurfaceStyle {
    Color fill = colors::transparent;
    Color border = colors::transparent;
    ShadowStyle shadow{};
    f32 border_width = 0.0f;
    f32 radius = 0.0f;
    BorderMode border_mode = BorderMode::None;
    bool draw_fill = false;
    UiNineSlice skin{};
    bool use_skin = false;
    Color skin_tint = colors::white;

    // --- advanced look (Layer B) ---
    SurfaceFill fill_kind = SurfaceFill::Solid;
    Gradient gradient{};                            // fill_kind == Gradient
    Color glass_tint = colors::transparent;         // fill_kind == Glass (also the degrade fill)
    f32 glass_blur = 0.0f;
    f32 glass_highlight = 0.0f;
    ShaderHandle shader{};                          // fill_kind == Shader
    ShaderParams shader_params{};
    BevelStyle bevel{};
    InnerShadowStyle inner_shadow{};
    Color glow_color = colors::transparent;         // focus/accent outset glow
    f32 glow_size = 0.0f;
    f32 opacity = 1.0f;                             // multiplies alpha of every painted color
};

struct InteractiveSurfaceStyle {
    SurfaceStyle normal{};
    SurfaceStyle hovered{};
    SurfaceStyle pressed{};
    SurfaceStyle focused{};
    SurfaceStyle disabled{};
    SurfaceStyle selected{};
};

} // namespace kin::ui2
