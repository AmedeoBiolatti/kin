#include <kin/renderer/render_queue.hpp>

#include <algorithm>
#include <cstdint>
#include <array>
#include <bit>
#include <cmath>
#include <utility>

namespace kin {

namespace {

bool has_material_tint(const Material2D* material, Color& out) {
    if (!material) {
        return false;
    }
    out = material->tint;
    return true;
}

// A camera that zooms or turns is the renderer's transform while the queue
// draws (CameraTransform); one that only moves shifts each command instead.
bool camera_transforms(const RenderView& view) {
    return view.camera && !view.output_space && !view.camera->translation_only();
}

Vec2f to_view_pos(Vec2f value, const RenderView& view) {
    if (view.output_space || !view.camera || camera_transforms(view)) {
        return value;
    }
    return view.camera->world_to_screen(value);
}

Rectf to_view_rect(Rectf rect, const RenderView& view) {
    const Vec2f pos = to_view_pos({rect.x, rect.y}, view);
    return {pos.x, pos.y, rect.w, rect.h};
}

// to_view_rect() with the camera's offset looked up once, for loops.
struct ViewShift {
    explicit ViewShift(const RenderView* view)
        : active(view && !view->output_space && view->camera && view->camera->translation_only()),
          offset(active ? view->camera->effective_offset() : Vec2f{}) {}

    Rectf apply(Rectf rect) const {
        return active ? Rectf{rect.x - offset.x, rect.y - offset.y, rect.w, rect.h} : rect;
    }

    bool active;
    Vec2f offset;
};

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

// Pushes the view's camera onto the renderer for a flush, when it zooms or turns.
class CameraTransform {
public:
    CameraTransform(Renderer2D& renderer, const RenderView& view) {
        if (camera_transforms(view)) {
            _renderer = &renderer;
            renderer.push_transform(view.camera->view_transform());
        }
    }
    ~CameraTransform() {
        if (_renderer) {
            _renderer->pop_transform();
        }
    }
    CameraTransform(const CameraTransform&) = delete;
    CameraTransform& operator=(const CameraTransform&) = delete;

private:
    Renderer2D* _renderer = nullptr;
};

// Takes the camera's transform off for one command: output-pixel commands and
// callbacks draw where they would under a camera that only moves.
class WithoutCamera {
public:
    WithoutCamera(Renderer2D& renderer, const RenderView& view) {
        if (camera_transforms(view)) {
            _renderer = &renderer;
            _transform = renderer.transform();
            renderer.set_transform(_transform * view.camera->view_transform().inverse());
        }
    }
    ~WithoutCamera() {
        if (_renderer) {
            _renderer->set_transform(_transform);
        }
    }
    WithoutCamera(const WithoutCamera&) = delete;
    WithoutCamera& operator=(const WithoutCamera&) = delete;

private:
    Renderer2D* _renderer = nullptr;
    Affine2 _transform{};
};

// Shared execution core. Takes the already-resolved geometry (rect for
// quad-like commands, a/b for lines) so callers can apply a view transform
// without copying the whole RenderCommand — RenderCommand owns strings, a
// std::function, and shared_ptr-backed textures, so a by-value copy per
// command per flush was the single hottest allocation/refcount path.
Color command_color(const RenderCommand& command) {
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
    return color;
}

void execute_resolved(Renderer2D& renderer, const RenderCommand& command, Rectf rect, Vec2f a, Vec2f b) {
    const Color color = command_color(command);

    switch (command.type) {
    case RenderCommandType::Clear:
        renderer.clear(command.color);
        break;
    case RenderCommandType::FillRect:
    case RenderCommandType::DrawRect: {
        // Turned about its pivot, through the renderer's transform.
        const bool turned = command.rotation != 0.0f;
        if (turned) {
            const Vec2f p{rect.x + rect.w * command.pivot.x, rect.y + rect.h * command.pivot.y};
            renderer.push_transform(Affine2::translation(p) * Affine2::rotation(command.rotation) *
                                    Affine2::translation({-p.x, -p.y}));
        }
        if (command.type == RenderCommandType::FillRect) {
            renderer.fill_rect(rect, color);
        } else {
            renderer.draw_rect(rect, color);
        }
        if (turned) {
            renderer.pop_transform();
        }
        break;
    }
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
        if (command.texture && command.source.w > 0.0f && command.source.h > 0.0f) {
            renderer.draw_texture(command.texture,
                                  command.source,
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

// Who queued a command or sprite (draw_trace.hpp); 0 without the render probe.
template<typename T>
u32 draw_source_of([[maybe_unused]] const T& item) {
#ifdef KIN_ENABLE_RENDER_PROBE
    return item.draw_source;
#else
    return 0;
#endif
}

// A Texture or Sprite command as the quad execute_render_command() would draw;
// false for other commands and for ones that draw nothing.
bool sprite_of(Renderer2D& renderer, const RenderCommand& command, const RenderView* view, SpriteInstance& out) {
    if (!command.texture) {
        return false;
    }
    if (command.type == RenderCommandType::Texture) {
        const Vec2i size = command.texture.size();
        out.source = command.source.w > 0.0f && command.source.h > 0.0f
            ? command.source
            : Rectf{0.0f, 0.0f, static_cast<f32>(size.x), static_cast<f32>(size.y)};
    } else if (command.type == RenderCommandType::Sprite && command.source.w > 0.0f && command.source.h > 0.0f) {
        out.source = command.source;
    } else {
        return false;
    }
    out.dest = command.output_pixel_rect ? output_to_logical(renderer, command.rect)
             : view                      ? to_view_rect(command.rect, *view)
                                         : command.rect;
    out.tint = command_color(command);
    out.rotation = command.rotation;
    out.pivot = command.pivot;
    return true;
}

// render_view_visible() with the view's rectangle worked out once, for loops.
class Culler {
public:
    explicit Culler(const RenderView& view)
        : _rect(render_view_visible_rect(view)),
          _enabled(view.culling_enabled && _rect.w > 0.0f && _rect.h > 0.0f) {}

    bool visible(Rectf bounds) const {
        return !_enabled || (bounds.x + bounds.w >= _rect.x && bounds.y + bounds.h >= _rect.y &&
                             bounds.x <= _rect.x + _rect.w && bounds.y <= _rect.y + _rect.h);
    }

private:
    Rectf _rect;
    bool _enabled;
};

// Gathers consecutive sprites that share a texture and draws them with one
// draw_sprites() call (instanced on the GPU backend); a lone sprite still goes
// through draw_texture(). Anything else drawn in between ends the run first, so
// the draw order never changes.
class SpriteRun {
public:
    SpriteRun(Renderer2D& renderer, std::vector<SpriteInstance>& sprites)
        : _renderer(renderer), _sprites(sprites) {
        _sprites.clear();
#ifdef KIN_ENABLE_RENDER_PROBE
        _trace = active_draw_trace();
#endif
    }

    // Takes the command if it is a sprite (drawing any run it cannot join), or
    // draws the pending run and returns false so the caller executes it.
    bool take(const RenderCommand& command, const RenderView* view) {
        if ((command.type != RenderCommandType::Texture && command.type != RenderCommandType::Sprite) ||
            (command.output_pixel_rect && view && camera_transforms(*view))) {
            flush();
            return false;
        }
        SpriteInstance sprite;
        if (sprite_of(_renderer, command, view, sprite)) {
            add(command.texture, sprite, draw_source_of(command));
        }
        return true; // a sprite, drawn or (if it draws nothing) dropped
    }

    void add(const Texture& texture, const SpriteInstance& sprite, [[maybe_unused]] u32 draw_source) {
        if (!_sprites.empty() && !(texture == *_texture)) {
            flush();
        }
        _texture = &texture;
        _sprites.push_back(sprite);
#ifdef KIN_ENABLE_RENDER_PROBE
        if (_trace) {
            _sources.push_back(draw_source);
        }
#endif
    }

    void flush() {
        if (_sprites.size() == 1) {
            const SpriteInstance& s = _sprites.front();
#ifdef KIN_ENABLE_RENDER_PROBE
            const DrawSourceScope scope{_trace ? _sources.front() : 0};
#endif
            _renderer.draw_texture(*_texture, s.source, s.dest, s.tint, s.rotation, s.pivot);
        } else if (!_sprites.empty()) {
#ifdef KIN_ENABLE_RENDER_PROBE
            if (_trace) {
                _trace->set_instance_sources(_sources);
            }
#endif
            _renderer.draw_sprites(*_texture, _sprites);
        }
        _sprites.clear();
#ifdef KIN_ENABLE_RENDER_PROBE
        _sources.clear();
#endif
    }

private:
    Renderer2D& _renderer;
    std::vector<SpriteInstance>& _sprites;
    const Texture* _texture = nullptr;
#ifdef KIN_ENABLE_RENDER_PROBE
    DrawTrace* _trace = nullptr;
    std::vector<u32> _sources; // per sprite, while tracing
#endif
};

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
    case RenderCommandType::Text:
    case RenderCommandType::PushViewport:
        return command.rect;
    case RenderCommandType::FillRect:
    case RenderCommandType::DrawRect:
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
#ifdef KIN_ENABLE_RENDER_PROBE
    const DrawSourceScope scope{command.draw_source};
#endif
    const Rectf rect = command.output_pixel_rect ? output_to_logical(renderer, command.rect) : command.rect;
    execute_resolved(renderer, command, rect, command.a, command.b);
}

void execute_render_command(Renderer2D& renderer, const RenderCommand& command, const RenderView& view) {
#ifdef KIN_ENABLE_RENDER_PROBE
    const DrawSourceScope scope{command.draw_source};
#endif
    // output_pixel_rect commands bypass the view/camera transform entirely.
    if (command.output_pixel_rect) {
        const WithoutCamera outside{renderer, view};
        execute_resolved(renderer, command, output_to_logical(renderer, command.rect), command.a, command.b);
        return;
    }
    if (command.type == RenderCommandType::Text || command.type == RenderCommandType::Custom) {
        const WithoutCamera outside{renderer, view};
        execute_resolved(renderer, command, command.rect, command.a, command.b);
        return;
    }

    // Transform only the geometry fields the view affects — no RenderCommand copy.
    Rectf rect = command.rect;
    Vec2f a = command.a;
    Vec2f b = command.b;
    switch (command.type) {
    case RenderCommandType::PushViewport:
        // Viewports are not transformed: one under a zooming or turning camera
        // covers where its rectangle lands on screen.
        rect = camera_transforms(view) ? transformed_bounds(view.camera->view_transform(), command.rect)
                                       : to_view_rect(command.rect, view);
        break;
    case RenderCommandType::FillRect:
    case RenderCommandType::DrawRect:
    case RenderCommandType::Text:
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
    _sprites.clear();
    _textures.clear();
    _sprites_after = 0;
    _next_sequence = 0;
    _sorted = true;
    _in_sequence = true;
}

void RenderQueue::reserve(std::size_t capacity) {
    _commands.reserve(capacity);
    _sprites.reserve(capacity);
}

void RenderQueue::submit(RenderCommand command) {
    // Plain sprites (e.g. particles) join the compact lane like draw_texture's.
    const bool sprite = command.texture && !command.material && !command.output_pixel_rect && !command.detail &&
                        command.a == Vec2f{} && command.b == Vec2f{} &&
                        (command.type == RenderCommandType::Texture ||
                         (command.type == RenderCommandType::Sprite && command.source.w > 0.0f && command.source.h > 0.0f));
#ifdef KIN_ENABLE_RENDER_PROBE
    if (command.draw_source == 0) {
        command.draw_source = current_draw_source();
    }
    const u32 draw_source = command.draw_source;
#else
    const u32 draw_source = 0;
#endif
    if (sprite) {
        queue_sprite(command.type, command.key, command.texture, command.source, command.rect, command.color,
                     command.rotation, command.pivot, draw_source);
        return;
    }
    command.sequence = _next_sequence++;
    _commands.push_back(std::move(command));
    _sorted = false;
}
void RenderQueue::submit(std::span<const RenderCommand> commands) {
    if (commands.empty()) {
        return;
    }
    const std::size_t first = _commands.size();
    _commands.insert(_commands.end(), commands.begin(), commands.end());
    for (std::size_t i = first; i < _commands.size(); ++i) {
        _commands[i].sequence = _next_sequence++;
    }
    _sorted = false;
}

void RenderQueue::clear_color(Color color) {
    submit({.type = RenderCommandType::Clear, .color = color});
}

void RenderQueue::fill_rect(RenderKey key, Rectf rect, Color color, MaterialRef material) {
    submit({.type = RenderCommandType::FillRect, .key = key, .rect = rect, .color = color, .material = material.material});
}

void RenderQueue::draw_rect(RenderKey key, Rectf rect, Color color) {
    submit({.type = RenderCommandType::DrawRect, .key = key, .rect = rect, .color = color});
}

void RenderQueue::draw_line(RenderKey key, Vec2f a, Vec2f b, Color color) {
    submit({.type = RenderCommandType::Line, .key = key, .a = a, .b = b, .color = color});
}

void RenderQueue::draw_texture(RenderKey key, const Texture& texture, Rectf dest, Color tint, MaterialRef material, f32 rotation, Vec2f pivot) {
    if (texture && !material.material) {
        queue_sprite(RenderCommandType::Texture, key, texture, {}, dest, tint, rotation, pivot);
        return;
    }
    submit({.type = RenderCommandType::Texture, .key = key, .rect = dest, .color = tint, .texture = texture, .rotation = rotation, .pivot = pivot, .material = material.material});
}

void RenderQueue::draw_sprite(RenderKey key, const Sprite& sprite, Rectf dest, Color tint, MaterialRef material, f32 rotation, Vec2f pivot) {
    if (sprite.texture && !material.material && sprite.source.w > 0.0f && sprite.source.h > 0.0f) {
        queue_sprite(RenderCommandType::Sprite, key, sprite.texture, sprite.source, dest, tint, rotation, pivot);
        return;
    }
    submit({.type = RenderCommandType::Sprite, .key = key, .rect = dest, .source = sprite.source, .color = tint, .texture = sprite.texture, .rotation = rotation, .pivot = pivot, .material = material.material});
}

void RenderQueue::draw_texture_region(RenderKey key, const Texture& texture, Rectf source, Rectf dest, Color tint, f32 rotation, Vec2f pivot) {
    if (texture) {
        queue_sprite(RenderCommandType::Texture, key, texture, source, dest, tint, rotation, pivot);
        return;
    }
    submit({.type = RenderCommandType::Texture, .key = key, .rect = dest, .source = source, .color = tint, .texture = texture, .rotation = rotation, .pivot = pivot});
}

void RenderQueue::append_sprites(std::span<const PreparedSprite> sprites) {
    _sprites.reserve(_sprites.size() + sprites.size());
    for (const PreparedSprite& sprite : sprites) {
        const bool drawable = sprite.texture && *sprite.texture &&
                              (sprite.type == RenderCommandType::Texture || (sprite.source.w > 0.0f && sprite.source.h > 0.0f));
#ifdef KIN_ENABLE_RENDER_PROBE
        const DrawSourceScope scope{sprite.draw_source};
#endif
        if (drawable) {
            queue_sprite(sprite.type, sprite.key, *sprite.texture, sprite.source, sprite.dest, sprite.tint, sprite.rotation,
                         sprite.pivot);
        } else {
            submit({.type = sprite.type, .key = sprite.key, .rect = sprite.dest, .source = sprite.source, .color = sprite.tint,
                    .texture = sprite.texture ? *sprite.texture : Texture{}, .rotation = sprite.rotation, .pivot = sprite.pivot});
        }
    }
}

RenderQueue::SpriteBlock RenderQueue::reserve_sprites(std::size_t count) {
    if (_sprites.empty()) {
        _sprites_after = _commands.size();
    }
    const SpriteBlock block{.first = _sprites.size(), .sequence = _next_sequence, .count = count};
    _sprites.resize(_sprites.size() + count);
    _next_sequence += count;
    _sorted = false;
    return block;
}

void RenderQueue::write_sprite(const SpriteBlock& block, std::size_t index, u32 texture, const PreparedSprite& sprite) {
    _sprites[block.first + index] = {
        .dest = sprite.dest,
        .source = sprite.source,
        .tint = sprite.tint,
        .rotation = sprite.rotation,
        .pivot = sprite.pivot,
        .layer = sprite.key.layer,
        .order = sprite.key.order,
        .y = sprite.key.y,
        .texture = texture,
        .pass_mask = sprite.key.pass_mask,
        .sequence = block.sequence + index,
        .use_y = sprite.key.use_y,
        .type = sprite.type,
#ifdef KIN_ENABLE_RENDER_PROBE
        .draw_source = sprite.draw_source != 0 ? sprite.draw_source : current_draw_source(),
#endif
    };
}

void RenderQueue::queue_sprite(RenderCommandType type, RenderKey key, const Texture& texture, Rectf source, Rectf dest,
                               Color tint, f32 rotation, Vec2f pivot, [[maybe_unused]] u32 draw_source) {
    if (_sprites.empty()) {
        _sprites_after = _commands.size();
    }
    _sprites.push_back({
        .dest = dest,
        .source = source,
        .tint = tint,
        .rotation = rotation,
        .pivot = pivot,
        .layer = key.layer,
        .order = key.order,
        .y = key.y,
        .texture = texture_slot(texture),
        .pass_mask = key.pass_mask,
        .sequence = _next_sequence++,
        .use_y = key.use_y,
        .type = type,
#ifdef KIN_ENABLE_RENDER_PROBE
        .draw_source = draw_source != 0 ? draw_source : current_draw_source(),
#endif
    });
    _sorted = false;
}

u32 RenderQueue::texture_slot(const Texture& texture) {
    // A small cache by texture address makes the lookup constant time however
    // many textures alternate; entries are verified, so a stale one only misses.
    const ITextureBackend* id = texture.backend().get();
    u32& cached = _texture_cache[(reinterpret_cast<std::uintptr_t>(id) >> 4) & (_texture_cache.size() - 1)];
    if (cached != 0 && cached <= _textures.size() && _textures[cached - 1].backend().get() == id) {
        return cached - 1;
    }
    u32 slot = static_cast<u32>(_textures.size());
    for (u32 i = 0; i < _textures.size(); ++i) {
        if (_textures[i].backend().get() == id) {
            slot = i;
            break;
        }
    }
    if (slot == _textures.size()) {
        _textures.push_back(texture);
    }
    cached = slot + 1;
    return slot;
}

RenderCommand RenderQueue::to_command(const QueuedSprite& sprite) const {
    return {
        .type = sprite.type,
#ifdef KIN_ENABLE_RENDER_PROBE
        .draw_source = sprite.draw_source,
#endif
        .key = {.layer = sprite.layer, .order = sprite.order, .y = sprite.y, .use_y = sprite.use_y, .pass_mask = sprite.pass_mask},
        .sequence = sprite.sequence,
        .rect = sprite.dest,
        .source = sprite.source,
        .color = sprite.tint,
        .texture = _textures[sprite.texture],
        .rotation = sprite.rotation,
        .pivot = sprite.pivot,
    };
}

template<typename Visit>
void RenderQueue::for_each_in_submission_order(Visit&& visit) const {
    // _commands before _sprites_after predate every queued sprite; after it, both
    // lists are in submission order, so they merge by sequence.
    std::size_t c = 0;
    for (; c < _sprites_after && c < _commands.size(); ++c) {
        visit(static_cast<u32>(c));
    }
    std::size_t q = 0;
    while (c < _commands.size() || q < _sprites.size()) {
        if (q == _sprites.size() || (c < _commands.size() && _commands[c].sequence < _sprites[q].sequence)) {
            visit(static_cast<u32>(c++));
        } else {
            visit(static_cast<u32>(q++) | sprite_bit);
        }
    }
}

void RenderQueue::materialize() const {
    if (_sprites.empty()) {
        return;
    }
    _sort_scratch.clear();
    _sort_scratch.reserve(_commands.size() + _sprites.size());
    for_each_in_submission_order([&](u32 index) {
        if (index & sprite_bit) {
            _sort_scratch.push_back(to_command(_sprites[index & ~sprite_bit]));
        } else {
            _sort_scratch.push_back(std::move(_commands[index]));
        }
    });
    _commands.swap(_sort_scratch);
    _sort_scratch.clear();
    _sprites.clear();
    _textures.clear();
    _sprites_after = 0;
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

namespace {

u32 sortable_i32(i32 value) {
    return static_cast<u32>(value) ^ 0x8000'0000u;
}

u32 sortable_f32(f32 value) {
    if (value == 0.0f) {
        value = 0.0f; // -0 and +0 compare equal, so they must encode equal
    }
    const u32 bits = std::bit_cast<u32>(value);
    return (bits & 0x8000'0000u) != 0 ? ~bits : bits | 0x8000'0000u;
}

// Below this many commands a comparison sort beats the radix sort's fixed cost.
constexpr std::size_t radix_sort_min = 256;

} // namespace

bool RenderQueue::compute_draw_order() {
    const std::size_t count = size();
    if (count < 2) {
        return false;
    }

    // Sort compact keys, not the heavy commands, which are moved at most once.
    // Entries start in submission order when _commands is, so a stable sort keeps
    // ties in sequence order exactly as `before` does.
    const bool layer_then_y = _sort == RenderSortMode::LayerThenY;
    _sort_keys.clear();
    _sort_keys.reserve(count);
    const auto key_entry = [&](i32 layer, i32 order, f32 y, bool use_y, u32 index) {
        _sort_keys.push_back({sortable_i32(order), layer_then_y ? sortable_f32(use_y ? y : 0.0f) : 0u,
                              sortable_i32(layer), index});
    };
    if (_sprites.empty()) {
        for (std::size_t i = 0; i < count; ++i) {
            const RenderKey& key = _commands[i].key;
            key_entry(key.layer, key.order, key.y, key.use_y, static_cast<u32>(i));
        }
    } else {
        for_each_in_submission_order([&](u32 index) {
            if (index & sprite_bit) {
                const QueuedSprite& sprite = _sprites[index & ~sprite_bit];
                key_entry(sprite.layer, sprite.order, sprite.y, sprite.use_y, index);
            } else {
                const RenderKey& key = _commands[index].key;
                key_entry(key.layer, key.order, key.y, key.use_y, index);
            }
        });
    }

    if (_in_sequence && count >= radix_sort_min) {
        // LSD radix sort over the key bytes, least significant first; stable, so
        // ties keep submission order. Words equal on every key (typically layer
        // and order) are skipped outright, and so is any byte equal on every key.
        const SortEntry& first = _sort_keys[0];
        bool varies[3] = {false, false, false}; // order, y, layer
        for (const SortEntry& entry : _sort_keys) {
            varies[0] = varies[0] || entry.order != first.order;
            varies[1] = varies[1] || entry.y != first.y;
            varies[2] = varies[2] || entry.layer != first.layer;
        }
        const auto word_of = [](const SortEntry& entry, std::size_t word) -> u32 {
            return word == 0 ? entry.order : word == 1 ? entry.y : entry.layer;
        };
        // Three passes per word, of 11, 11 and 10 bits.
        constexpr std::array<u32, 3> shifts{0, 11, 22};
        constexpr u32 digit_mask = 0x7ffu;
        _radix_scratch.resize(count);
        for (std::size_t word = 0; word < 3; ++word) {
            if (!varies[word]) {
                continue;
            }
            std::array<std::array<u32, 2048>, 3> counts{};
            for (const SortEntry& entry : _sort_keys) {
                const u32 value = word_of(entry, word);
                ++counts[0][value & digit_mask];
                ++counts[1][(value >> 11) & digit_mask];
                ++counts[2][value >> 22];
            }
            for (std::size_t digit = 0; digit < 3; ++digit) {
                std::array<u32, 2048>& bucket = counts[digit];
                const u32 shift = shifts[digit];
                if (bucket[(word_of(_sort_keys[0], word) >> shift) & digit_mask] == count) {
                    continue;
                }
                u32 offset = 0;
                for (u32& slot : bucket) {
                    const u32 n = slot;
                    slot = offset;
                    offset += n;
                }
                for (const SortEntry& entry : _sort_keys) {
                    _radix_scratch[bucket[(word_of(entry, word) >> shift) & digit_mask]++] = entry;
                }
                _sort_keys.swap(_radix_scratch);
            }
        }
    } else {
        const auto sequence_of = [&](u32 index) {
            return (index & sprite_bit) ? _sprites[index & ~sprite_bit].sequence : _commands[index].sequence;
        };
        std::sort(_sort_keys.begin(), _sort_keys.end(), [&](const SortEntry& a, const SortEntry& b) {
            if (a.layer != b.layer) {
                return a.layer < b.layer;
            }
            if (a.y != b.y) {
                return a.y < b.y;
            }
            if (a.order != b.order) {
                return a.order < b.order;
            }
            return sequence_of(a.index) < sequence_of(b.index);
        });
    }

    if (!_sprites.empty()) {
        return true;
    }
    for (std::size_t i = 0; i < count; ++i) {
        if (_sort_keys[i].index != i) {
            return true;
        }
    }
    _sorted = true; // already physically in draw order
    return false;
}

void RenderQueue::sort_commands() {
    if (_sort == RenderSortMode::Submission || (_sorted && _sprites.empty()) || !compute_draw_order()) {
        materialize();
        return;
    }

    // Build the draw order once into a reused scratch buffer, converting queued
    // sprites on the way, then swap.
    _sort_scratch.clear();
    _sort_scratch.reserve(size());
    for (const SortEntry& entry : _sort_keys) {
        if (entry.index & sprite_bit) {
            _sort_scratch.push_back(to_command(_sprites[entry.index & ~sprite_bit]));
        } else {
            _sort_scratch.push_back(std::move(_commands[entry.index]));
        }
    }
    _commands.swap(_sort_scratch);
    _sort_scratch.clear();
    _sprites.clear();
    _textures.clear();
    _sprites_after = 0;
    _sorted = true;
    _in_sequence = false;
}

template<typename Draw>
void RenderQueue::for_each_in_draw_order(Draw&& draw) {
    const auto visit = [&](u32 index) {
        if (index & sprite_bit) {
            draw(static_cast<const RenderCommand*>(nullptr), &_sprites[index & ~sprite_bit]);
        } else {
            draw(&_commands[index], static_cast<const QueuedSprite*>(nullptr));
        }
    };
    if (_sort == RenderSortMode::Submission || (_sorted && _sprites.empty()) || !compute_draw_order()) {
        for_each_in_submission_order(visit);
        return;
    }
    for (const SortEntry& entry : _sort_keys) {
        visit(entry.index);
    }
}

namespace {

// The quad a queued sprite draws, shifted into the view.
SpriteInstance sprite_instance(Rectf dest, Rectf source, const Texture& texture, Color tint, f32 rotation, Vec2f pivot,
                               const ViewShift& shift) {
    if (source.w <= 0.0f || source.h <= 0.0f) {
        const Vec2i size = texture.size();
        source = {0.0f, 0.0f, static_cast<f32>(size.x), static_cast<f32>(size.y)};
    }
    return {.dest = shift.apply(dest), .source = source, .tint = tint, .rotation = rotation, .pivot = pivot};
}

} // namespace

void RenderQueue::cull(const RenderView& view) {
    cull(view, 0);
}

void RenderQueue::cull(const RenderView& view, u64 first, u64 last) {
    if (!view.culling_enabled || first >= last) {
        return;
    }
    const auto in_range = [&](u64 sequence) { return sequence >= first && sequence < last; };
    // Compact _commands in place, keeping _sprites_after on the same command.
    std::size_t kept = 0;
    std::size_t kept_before_sprites = 0;
    for (std::size_t i = 0; i < _commands.size(); ++i) {
        if (in_range(_commands[i].sequence) && !render_command_visible(_commands[i], view)) {
            continue;
        }
        if (kept != i) {
            _commands[kept] = std::move(_commands[i]);
        }
        kept_before_sprites += i < _sprites_after ? 1 : 0;
        ++kept;
    }
    _commands.erase(_commands.begin() + static_cast<std::ptrdiff_t>(kept), _commands.end());
    _sprites_after = kept_before_sprites;
    const Culler culler{view};
    std::erase_if(_sprites, [&](const QueuedSprite& sprite) {
        return in_range(sprite.sequence) && !culler.visible(rotated_bounds(sprite.dest, sprite.rotation, sprite.pivot));
    });
}

void RenderQueue::flush(Renderer2D& renderer) {
    flush(renderer, render_pass_mask::all);
}

void RenderQueue::flush(Renderer2D& renderer, u64 pass_mask) {
    SpriteRun run{renderer, _sprite_run};
    const ViewShift no_shift{nullptr};
    for_each_in_draw_order([&](const RenderCommand* command, const QueuedSprite* sprite) {
        if (sprite) {
            if ((sprite->pass_mask & pass_mask) != 0) {
                const Texture& texture = _textures[sprite->texture];
                run.add(texture, sprite_instance(sprite->dest, sprite->source, texture, sprite->tint, sprite->rotation,
                                                 sprite->pivot, no_shift),
                        draw_source_of(*sprite));
            }
            return;
        }
        if ((command->key.pass_mask & pass_mask) != 0 && !run.take(*command, nullptr)) {
            execute_render_command(renderer, *command);
        }
    });
    run.flush();
}

void RenderQueue::flush(Renderer2D& renderer, const RenderView& view) {
    flush(renderer, view, render_pass_mask::all);
}

void RenderQueue::flush(Renderer2D& renderer, const RenderView& view, u64 pass_mask) {
    const CameraTransform camera{renderer, view};
    SpriteRun run{renderer, _sprite_run};
    const Culler culler{view};
    const ViewShift shift{&view};
    for_each_in_draw_order([&](const RenderCommand* command, const QueuedSprite* sprite) {
        if (sprite) {
            if ((sprite->pass_mask & pass_mask) == 0 ||
                !culler.visible(rotated_bounds(sprite->dest, sprite->rotation, sprite->pivot))) {
                return;
            }
            const Texture& texture = _textures[sprite->texture];
            run.add(texture, sprite_instance(sprite->dest, sprite->source, texture, sprite->tint, sprite->rotation,
                                             sprite->pivot, shift),
                    draw_source_of(*sprite));
            return;
        }
        if ((command->key.pass_mask & pass_mask) == 0) {
            return;
        }
        if (view.culling_enabled && !render_command_visible(*command, view)) {
            return;
        }
        if (!run.take(*command, &view)) {
            execute_render_command(renderer, *command, view);
        }
    });
    run.flush();
}

void RenderQueue::flush_presorted(Renderer2D& renderer, const RenderView& view, u64 pass_mask) const {
    materialize();
    const CameraTransform camera{renderer, view};
    SpriteRun run{renderer, _sprite_run};
    for (const RenderCommand& command : _commands) {
        if ((command.key.pass_mask & pass_mask) == 0) {
            continue;
        }
        if (view.culling_enabled && !render_command_visible(command, view)) {
            continue;
        }
        if (!run.take(command, &view)) {
            execute_render_command(renderer, command, view);
        }
    }
    run.flush();
}

void RenderQueue::flush_merged_presorted(Renderer2D& renderer,
                                         std::span<const RenderCommand> other,
                                         const RenderView& view,
                                         u64 pass_mask) const {
    materialize();
    std::size_t lhs = 0;
    std::size_t rhs = 0;
    const CameraTransform camera{renderer, view};
    SpriteRun run{renderer, _sprite_run};
    const auto draw = [&](const RenderCommand& command) {
        if ((command.key.pass_mask & pass_mask) == 0) {
            return;
        }
        if (view.culling_enabled && !render_command_visible(command, view)) {
            return;
        }
        if (!run.take(command, &view)) {
            execute_render_command(renderer, command, view);
        }
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
    run.flush();
}

} // namespace kin
