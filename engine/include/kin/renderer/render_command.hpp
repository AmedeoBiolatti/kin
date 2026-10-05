#pragma once

#include <kin/renderer/material.hpp>
#include <kin/renderer/render_layer.hpp>
#include <kin/renderer/render_view.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <kin/renderer/sprite.hpp>

#include <functional>
#include <memory>
#include <string>

namespace kin {

namespace render_pass_mask {
constexpr u64 world = u64{1} << 0;
constexpr u64 effects = u64{1} << 1;
constexpr u64 ui = u64{1} << 2;
constexpr u64 debug = u64{1} << 3;
constexpr u64 material_albedo = u64{1} << 8;
constexpr u64 material_normal = u64{1} << 9;
constexpr u64 material_surface = u64{1} << 10;
constexpr u64 lighting = u64{1} << 11;
constexpr u64 shadow = u64{1} << 12;
constexpr u64 post = u64{1} << 13;
constexpr u64 editor_overlay = u64{1} << 14;
constexpr u64 all = ~u64{0};
} // namespace render_pass_mask

enum class RenderSortMode {
    Submission,
    LayerThenOrder,
    LayerThenY,
};

struct RenderKey {
    i32 layer = layer_value(RenderLayer::World);
    i32 order = 0;
    f32 y = 0.0f;
    bool use_y = false;
    u64 pass_mask = render_pass_mask::world;
};

enum class RenderCommandType : u8 {
    Clear,
    FillRect,
    DrawRect,
    Line,
    Texture,
    Sprite,
    Text,
    PushViewport,
    PopViewport,
    Custom,
};

// Cold per-command payload, needed only by Text/Custom commands. Kept behind a
// pointer (null on the common Sprite/Texture/Rect/Line path) so the hot RenderCommand
// stays small and cheap to construct, move (sort gather), and cache.
struct RenderCommandDetail {
    std::string text;
    Vec2f text_pos{};
    f32 text_scale = 1.0f;
    std::string debug_name;
    std::function<void(Renderer2D&)> callback;
};

// One queued draw. Kept small (checked below) because queues hold thousands and
// sorting, culling and flushing walk them every frame: fields are shared between
// command types rather than each type getting its own.
struct RenderCommand {
    RenderCommandType type = RenderCommandType::FillRect;
#ifdef KIN_ENABLE_RENDER_PROBE
    u32 draw_source = 0; // draw_trace.hpp: who queued it (fits in padding)
#endif
    RenderKey key{};
    u64 sequence = 0;
    Rectf rect{};                   // destination; unused by Line, Clear, PopViewport, Custom
    bool output_pixel_rect = false; // rect is in output pixels, not world/logical units
    Rectf source{};                 // Texture/Sprite: region of `texture`; empty means all of it (Texture only)
    Vec2f a{};                      // Line endpoints
    Vec2f b{};
    Color color = colors::white;
    Texture texture;                // Texture and Sprite commands
    f32 rotation = 0.0f;
    Vec2f pivot{0.5f, 0.5f};
    const Material2D* material = nullptr;
    // Text/debug/callback for Text & Custom commands; null on every other command.
    std::shared_ptr<RenderCommandDetail> detail;
};

// Was 216 bytes with a Sprite (a second Texture) and a MaterialRef (a string).
static_assert(sizeof(RenderCommand) <= 152, "RenderCommand grew; keep queued commands small");

Rectf render_command_bounds(const RenderCommand& command);
bool render_command_visible(const RenderCommand& command, const RenderView& view);
void execute_render_command(Renderer2D& renderer, const RenderCommand& command);
void execute_render_command(Renderer2D& renderer, const RenderCommand& command, const RenderView& view);

} // namespace kin
