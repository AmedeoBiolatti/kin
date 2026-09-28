#include <kin/renderer/render_queue.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace kin {

namespace {

bool has_material_tint(const MaterialRef& material, Color& out) {
    if (!material.material) {
        return false;
    }
    out = material.material->tint;
    return true;
}

Vec2f to_view_pos(Vec2f value, const RenderView& view) {
    if (view.output_space || !view.camera) {
        return value;
    }
    return view.camera->world_to_screen(value);
}

Rectf to_view_rect(Rectf rect, const RenderView& view) {
    const Vec2f pos = to_view_pos({rect.x, rect.y}, view);
    return {pos.x, pos.y, rect.w, rect.h};
}

Rectf rotated_bounds(Rectf rect, f32 rotation, Vec2f pivot) {
    if (rotation == 0.0f || rect.w <= 0.0f || rect.h <= 0.0f) {
        return rect;
    }

    constexpr f32 pi = 3.14159265358979323846f;
    const f32 radians = rotation * pi / 180.0f;
    const f32 c = std::cos(radians);
    const f32 s = std::sin(radians);
    const Vec2f center{rect.x + rect.w * pivot.x, rect.y + rect.h * pivot.y};
    const std::array<Vec2f, 4> corners{{
        {rect.x, rect.y},
        {rect.x + rect.w, rect.y},
        {rect.x + rect.w, rect.y + rect.h},
        {rect.x, rect.y + rect.h},
    }};

    f32 min_x = 0.0f;
    f32 max_x = 0.0f;
    f32 min_y = 0.0f;
    f32 max_y = 0.0f;
    bool first = true;
    for (Vec2f corner : corners) {
        const Vec2f delta{corner.x - center.x, corner.y - center.y};
        const Vec2f rotated{
            center.x + delta.x * c - delta.y * s,
            center.y + delta.x * s + delta.y * c,
        };
        if (first) {
            min_x = max_x = rotated.x;
            min_y = max_y = rotated.y;
            first = false;
        } else {
            min_x = std::min(min_x, rotated.x);
            max_x = std::max(max_x, rotated.x);
            min_y = std::min(min_y, rotated.y);
            max_y = std::max(max_y, rotated.y);
        }
    }

    return {min_x, min_y, max_x - min_x, max_y - min_y};
}

Rectf output_to_logical(Renderer2D& renderer, Rectf rect) {
    const Vec2f min = renderer.window_to_logical({rect.x, rect.y});
    const Vec2f max = renderer.window_to_logical({rect.x + rect.w, rect.y + rect.h});
    return Rectf{min.x, min.y, max.x - min.x, max.y - min.y};
}

// Shared execution core. Takes the already-resolved geometry (rect for
// quad-like commands, a/b for lines) so callers can apply a view transform
// without copying the whole RenderCommand — RenderCommand owns strings, a
// std::function, and shared_ptr-backed textures, so a by-value copy per
// command per flush was the single hottest allocation/refcount path.
void execute_resolved(Renderer2D& renderer, const RenderCommand& command, Rectf rect, Vec2f a, Vec2f b) {
    Color color = command.color;
    Color material_tint{};
    if (has_material_tint(command.material, material_tint)) {
        color = {
            static_cast<u8>((static_cast<u32>(color.r) * material_tint.r) / 255),
            static_cast<u8>((static_cast<u32>(color.g) * material_tint.g) / 255),
            static_cast<u8>((static_cast<u32>(color.b) * material_tint.b) / 255),
            static_cast<u8>((static_cast<u32>(color.a) * material_tint.a) / 255),
        };
    }

    switch (command.type) {
    case RenderCommandType::Clear:
        renderer.clear(command.color);
        break;
    case RenderCommandType::FillRect:
        renderer.fill_rect(rect, color);
        break;
    case RenderCommandType::DrawRect:
        renderer.draw_rect(rect, color);
        break;
    case RenderCommandType::Line:
        renderer.draw_line(a, b, color);
        break;
    case RenderCommandType::Texture:
        if (command.texture) {
            const Vec2i size = command.texture.size();
            const Rectf source = command.source.w > 0.0f && command.source.h > 0.0f
                ? command.source
                : Rectf{0.0f, 0.0f, static_cast<f32>(size.x), static_cast<f32>(size.y)};
            renderer.draw_texture(command.texture,
                                  source,
                                  rect,
                                  color,
                                  command.rotation,
                                  command.pivot);
        }
        break;
    case RenderCommandType::Sprite:
        if (command.sprite.valid()) {
            renderer.draw_texture(command.sprite.texture,
                                  command.sprite.source,
                                  rect,
                                  color,
                                  command.rotation,
                                  command.pivot);
        }
        break;
    case RenderCommandType::Text:
        if (command.detail && command.detail->callback) {
            command.detail->callback(renderer);
        }
        break;
    case RenderCommandType::PushViewport:
        renderer.push_viewport(rect);
        break;
    case RenderCommandType::PopViewport:
        renderer.pop_viewport();
        break;
    case RenderCommandType::Custom:
        if (command.detail && command.detail->callback) {
            command.detail->callback(renderer);
        }
        break;
    }
}

} // namespace

Rectf render_command_bounds(const RenderCommand& command) {
    switch (command.type) {
    case RenderCommandType::Clear:
    case RenderCommandType::PopViewport:
    case RenderCommandType::Custom:
        return {};
    case RenderCommandType::Line:
        return {
            std::min(command.a.x, command.b.x),
            std::min(command.a.y, command.b.y),
            std::abs(command.a.x - command.b.x),
            std::abs(command.a.y - command.b.y),
        };
    case RenderCommandType::FillRect:
    case RenderCommandType::DrawRect:
    case RenderCommandType::Text:
    case RenderCommandType::PushViewport:
        return command.rect;
    case RenderCommandType::Texture:
    case RenderCommandType::Sprite:
        return rotated_bounds(command.rect, command.rotation, command.pivot);
    }
    return {};
}

bool render_command_visible(const RenderCommand& command, const RenderView& view) {
    if (command.type == RenderCommandType::Clear ||
        command.type == RenderCommandType::PushViewport ||
        command.type == RenderCommandType::PopViewport ||
        command.type == RenderCommandType::Custom ||
        command.output_pixel_rect) {
        return true;
    }
    return render_view_visible(view, render_command_bounds(command));
}

void execute_render_command(Renderer2D& renderer, const RenderCommand& command) {
    const Rectf rect = command.output_pixel_rect ? output_to_logical(renderer, command.rect) : command.rect;
    execute_resolved(renderer, command, rect, command.a, command.b);
}

void execute_render_command(Renderer2D& renderer, const RenderCommand& command, const RenderView& view) {
    // output_pixel_rect commands bypass the view/camera transform entirely.
    if (command.output_pixel_rect) {
        execute_resolved(renderer, command, output_to_logical(renderer, command.rect), command.a, command.b);
        return;
    }

    // Transform only the geometry fields the view affects — no RenderCommand copy.
    Rectf rect = command.rect;
    Vec2f a = command.a;
    Vec2f b = command.b;
    switch (command.type) {
    case RenderCommandType::FillRect:
    case RenderCommandType::DrawRect:
    case RenderCommandType::Text:
    case RenderCommandType::PushViewport:
    case RenderCommandType::Texture:
    case RenderCommandType::Sprite:
        rect = to_view_rect(command.rect, view);
        break;
    case RenderCommandType::Line:
        a = to_view_pos(command.a, view);
        b = to_view_pos(command.b, view);
        break;
    case RenderCommandType::Clear:
    case RenderCommandType::PopViewport:
    case RenderCommandType::Custom:
        break;
    }
    execute_resolved(renderer, command, rect, a, b);
}

RenderQueue::RenderQueue(RenderSortMode sort)
    : _sort(sort) {
}

void RenderQueue::clear() {
    _commands.clear();
    _next_sequence = 0;
}

void RenderQueue::reserve(std::size_t capacity) {
    _commands.reserve(capacity);
}

void RenderQueue::submit(RenderCommand command) {
    command.sequence = _next_sequence++;
    _commands.push_back(std::move(command));
}

void RenderQueue::clear_color(Color color) {
    submit({.type = RenderCommandType::Clear, .color = color});
}

void RenderQueue::fill_rect(RenderKey key, Rectf rect, Color color, MaterialRef material) {
    submit({.type = RenderCommandType::FillRect, .key = key, .rect = rect, .color = color, .material = std::move(material)});
}

void RenderQueue::draw_rect(RenderKey key, Rectf rect, Color color) {
    submit({.type = RenderCommandType::DrawRect, .key = key, .rect = rect, .color = color});
}

void RenderQueue::draw_line(RenderKey key, Vec2f a, Vec2f b, Color color) {
    submit({.type = RenderCommandType::Line, .key = key, .a = a, .b = b, .color = color});
}

void RenderQueue::draw_texture(RenderKey key, const Texture& texture, Rectf dest, Color tint, MaterialRef material, f32 rotation, Vec2f pivot) {
    submit({.type = RenderCommandType::Texture, .key = key, .rect = dest, .color = tint, .texture = texture, .rotation = rotation, .pivot = pivot, .material = std::move(material)});
}

void RenderQueue::draw_sprite(RenderKey key, const Sprite& sprite, Rectf dest, Color tint, MaterialRef material, f32 rotation, Vec2f pivot) {
    submit({.type = RenderCommandType::Sprite, .key = key, .rect = dest, .color = tint, .sprite = sprite, .rotation = rotation, .pivot = pivot, .material = std::move(material)});
}

void RenderQueue::draw_text(RenderKey key,
                            std::string text,
                            Vec2f pos,
                            Rectf bounds,
                            f32 scale,
                            Color color,
                            std::function<void(Renderer2D&)> callback) {
    RenderCommand command{
        .type = RenderCommandType::Text,
        .key = key,
        .rect = bounds,
        .color = color,
    };
    command.detail = std::make_shared<RenderCommandDetail>(RenderCommandDetail{
        .text = std::move(text),
        .text_pos = pos,
        .text_scale = scale,
        .callback = std::move(callback),
    });
    submit(std::move(command));
}

void RenderQueue::push_viewport(Rectf rect) {
    submit({.type = RenderCommandType::PushViewport, .rect = rect});
}

void RenderQueue::pop_viewport() {
    submit({.type = RenderCommandType::PopViewport});
}

void RenderQueue::custom(RenderKey key, std::function<void(Renderer2D&)> callback, std::string debug_name) {
    RenderCommand command{
        .type = RenderCommandType::Custom,
        .key = key,
    };
    command.detail = std::make_shared<RenderCommandDetail>(RenderCommandDetail{
        .debug_name = std::move(debug_name),
        .callback = std::move(callback),
    });
    submit(std::move(command));
}

bool RenderQueue::before(const RenderCommand& a, const RenderCommand& b) const {
    if (_sort == RenderSortMode::Submission) {
        return a.sequence < b.sequence;
    }
    if (a.key.layer != b.key.layer) {
        return a.key.layer < b.key.layer;
    }
    if (_sort == RenderSortMode::LayerThenY) {
        const f32 ay = a.key.use_y ? a.key.y : 0.0f;
        const f32 by = b.key.use_y ? b.key.y : 0.0f;
        if (ay != by) {
            return ay < by;
        }
    }
    if (a.key.order != b.key.order) {
        return a.key.order < b.key.order;
    }
    return a.sequence < b.sequence;
}

void RenderQueue::sort_commands() {
    if (_sort == RenderSortMode::Submission || _commands.size() < 2) {
        return;
    }
    const std::size_t count = _commands.size();

    // Sort compact keys, not the heavy commands. `before` is a total order (sequence
    // is a unique tiebreaker), so a non-stable sort produces the identical ordering
    // the previous stable_sort did — without the O(n log n) temp buffer and 300-byte
    // element moves.
    _sort_keys.clear();
    _sort_keys.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const RenderCommand& command = _commands[i];
        _sort_keys.push_back({command.key.layer, command.key.order, command.key.y,
                              command.key.use_y, command.sequence, static_cast<u32>(i)});
    }

    const bool layer_then_y = _sort == RenderSortMode::LayerThenY;
    std::sort(_sort_keys.begin(), _sort_keys.end(), [layer_then_y](const SortEntry& a, const SortEntry& b) {
        if (a.layer != b.layer) {
            return a.layer < b.layer;
        }
        if (layer_then_y) {
            const f32 ay = a.use_y ? a.y : 0.0f;
            const f32 by = b.use_y ? b.y : 0.0f;
            if (ay != by) {
                return ay < by;
            }
        }
        if (a.order != b.order) {
            return a.order < b.order;
        }
        return a.sequence < b.sequence;
    });

    // Materialize the permutation once into a reused scratch buffer, then swap.
    _sort_scratch.clear();
    _sort_scratch.reserve(count);
    for (const SortEntry& entry : _sort_keys) {
        _sort_scratch.push_back(std::move(_commands[entry.index]));
    }
    _commands.swap(_sort_scratch);
}

void RenderQueue::cull(const RenderView& view) {
    if (!view.culling_enabled) {
        return;
    }
    std::erase_if(_commands, [&](const RenderCommand& command) {
        return !render_command_visible(command, view);
    });
}

void RenderQueue::flush(Renderer2D& renderer) {
    flush(renderer, render_pass_mask::all);
}

void RenderQueue::flush(Renderer2D& renderer, u64 pass_mask) {
    sort_commands();
    for (const RenderCommand& command : _commands) {
        if ((command.key.pass_mask & pass_mask) == 0) {
            continue;
        }
        execute_render_command(renderer, command);
    }
}

void RenderQueue::flush(Renderer2D& renderer, const RenderView& view) {
    flush(renderer, view, render_pass_mask::all);
}

void RenderQueue::flush(Renderer2D& renderer, const RenderView& view, u64 pass_mask) {
    sort_commands();
    for (const RenderCommand& command : _commands) {
        if ((command.key.pass_mask & pass_mask) == 0) {
            continue;
        }
        if (view.culling_enabled && !render_command_visible(command, view)) {
            continue;
        }
        execute_render_command(renderer, command, view);
    }
}

void RenderQueue::flush_presorted(Renderer2D& renderer, const RenderView& view, u64 pass_mask) const {
    for (const RenderCommand& command : _commands) {
        if ((command.key.pass_mask & pass_mask) == 0) {
            continue;
        }
        if (view.culling_enabled && !render_command_visible(command, view)) {
            continue;
        }
        execute_render_command(renderer, command, view);
    }
}

void RenderQueue::flush_merged_presorted(Renderer2D& renderer,
                                         std::span<const RenderCommand> other,
                                         const RenderView& view,
                                         u64 pass_mask) const {
    std::size_t lhs = 0;
    std::size_t rhs = 0;
    const auto draw = [&](const RenderCommand& command) {
        if ((command.key.pass_mask & pass_mask) == 0) {
            return;
        }
        if (view.culling_enabled && !render_command_visible(command, view)) {
            return;
        }
        execute_render_command(renderer, command, view);
    };

    while (lhs < _commands.size() && rhs < other.size()) {
        if (before(other[rhs], _commands[lhs])) {
            draw(other[rhs++]);
        } else {
            draw(_commands[lhs++]);
        }
    }
    while (lhs < _commands.size()) {
        draw(_commands[lhs++]);
    }
    while (rhs < other.size()) {
        draw(other[rhs++]);
    }
}

} // namespace kin
