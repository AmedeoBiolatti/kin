#include <kin/scene/transitions.hpp>

#include <kin/renderer/renderer2d.hpp>
#include <kin/scene/scene_manager.hpp>

#include <algorithm>
#include <memory>
#include <utility>

namespace kin {
namespace {

f32 normalized_time(f32 elapsed, f32 duration) {
    return std::clamp(elapsed / std::max(duration, 0.001f), 0.0f, 1.0f);
}

void draw_fade(Renderer2D& renderer, Color color, f32 alpha) {
    const Vec2i size = renderer.output_size();
    color.a = static_cast<u8>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f);
    renderer.fill_rect(
        {0.0f, 0.0f, static_cast<f32>(size.x), static_cast<f32>(size.y)},
        color
    );
}

BuiltinShader builtin_for(TransitionKind kind) {
    switch (kind) {
    case TransitionKind::Pixelate: return BuiltinShader::TransitionPixelate;
    case TransitionKind::Wipe: return BuiltinShader::TransitionWipe;
    case TransitionKind::Iris: return BuiltinShader::TransitionIris;
    case TransitionKind::Crossfade: return BuiltinShader::TransitionCrossfade;
    case TransitionKind::Dissolve:
    case TransitionKind::Fade:
    default: return BuiltinShader::TransitionDissolve;
    }
}

// Pack a morph shader's uniforms: color_a = cover color (0..1), misc = (progress,
// direction, aspect). direction: 0 = out (scene->color), 1 = in (color->scene).
ShaderParams morph_params(Color color, f32 progress, f32 direction, f32 aspect) {
    ShaderParams p;
    p.uniforms[0] = static_cast<f32>(color.r) / 255.0f;
    p.uniforms[1] = static_cast<f32>(color.g) / 255.0f;
    p.uniforms[2] = static_cast<f32>(color.b) / 255.0f;
    p.uniforms[3] = 1.0f;
    p.uniforms[8] = progress;
    p.uniforms[9] = direction;
    p.uniforms[10] = aspect;
    return p;
}

Rectf full_screen(Renderer2D& r) {
    const Vec2i sz = r.output_size();
    return {0.0f, 0.0f, static_cast<f32>(sz.x), static_cast<f32>(sz.y)};
}

PooledTarget capture_full(Renderer2D& r) {
    return r.capture_backdrop(full_screen(r));
}

// In phase: morph the incoming scene up from `color` (Dissolve/Pixelate/Wipe/Iris), or
// cross-dissolve the outgoing snapshot into the incoming one (Crossfade). Pops when done.
class TransitionInScene final : public Scene {
public:
    TransitionInScene(TransitionKind kind, f32 duration, Color color, PooledTarget snapshot_a)
        : _kind(kind), _duration(std::max(duration, 0.001f)), _color(color),
          _snapshot_a(std::move(snapshot_a)) {}

    std::string_view name() const override { return "TransitionInScene"; }
    bool is_overlay() const override { return true; }
    bool updates_below() const override { return false; }

    void update(SceneContext& ctx) override {
        _elapsed += std::max(ctx.dt, 0.0f);
        if (_elapsed >= _duration) {
            ctx.scenes.pop();
        }
    }

    void render(SceneContext& ctx) override {
        Renderer2D& r = ctx.renderer;
        const Rectf full = full_screen(r);
        const f32 t = normalized_time(_elapsed, _duration);
        const bool gpu = r.capabilities().materials_2d;
        if (gpu && !_captured) {
            _snapshot_b = capture_full(r); // the incoming scene's first frame
            _captured = true;
        }
        const ShaderHandle sh = gpu ? r.builtin_shader(builtin_for(_kind)) : ShaderHandle{};
        if (gpu && sh && _snapshot_b.valid()) {
            if (_kind == TransitionKind::Crossfade && _snapshot_a.valid()) {
                ShaderParams p;
                p.uniforms[8] = t; // progress A -> B
                r.draw_shader_surface(full, sh, p, _snapshot_a.texture(), _snapshot_b.texture());
            } else {
                const f32 aspect = full.h > 0.0f ? full.w / full.h : 1.0f;
                r.draw_shader_surface(full, sh, morph_params(_color, t, 1.0f, aspect),
                                      _snapshot_b.texture());
            }
        } else {
            draw_fade(r, _color, 1.0f - t); // degrade: reveal from color
        }
    }

private:
    TransitionKind _kind;
    f32 _duration;
    f32 _elapsed = 0.0f;
    Color _color;
    PooledTarget _snapshot_a; // outgoing snapshot (Crossfade only)
    PooledTarget _snapshot_b; // incoming snapshot
    bool _captured = false;
};

// Out phase / orchestrator: morph the outgoing scene down to `color`, then swap to the
// next scene and push the in phase. Crossfade swaps after one frame (it blends A<->B in
// the in phase) so the outgoing frame is held just long enough to snapshot it.
class TransitionOutScene final : public Scene {
public:
    TransitionOutScene(std::unique_ptr<Scene> next, TransitionKind kind, f32 duration, Color color)
        : _next(std::move(next)), _kind(kind), _duration(std::max(duration, 0.001f)), _color(color) {}

    std::string_view name() const override { return "TransitionOutScene"; }
    bool is_overlay() const override { return true; }
    bool updates_below() const override { return false; }

    void update(SceneContext& ctx) override {
        _elapsed += std::max(ctx.dt, 0.0f);
        const bool gpu = ctx.renderer.capabilities().materials_2d;
        const f32 phase = _kind == TransitionKind::Crossfade ? 0.0f : _duration * 0.5f;
        // Wait until the outgoing frame has been snapshotted (gpu) before swapping.
        if ((!gpu || _captured) && _elapsed >= phase) {
            const f32 in_dur = _kind == TransitionKind::Crossfade ? _duration : _duration * 0.5f;
            ctx.scenes.pop();
            ctx.scenes.replace(std::move(_next));
            ctx.scenes.push(std::make_unique<TransitionInScene>(_kind, in_dur, _color,
                                                                std::move(_snapshot)));
        }
    }

    void render(SceneContext& ctx) override {
        Renderer2D& r = ctx.renderer;
        const bool gpu = r.capabilities().materials_2d;
        if (gpu && !_captured) {
            _snapshot = capture_full(r); // the outgoing scene's frame
            _captured = true;
        }
        if (_kind == TransitionKind::Crossfade) {
            return; // hold the live outgoing scene for the single pre-swap frame
        }
        const f32 phase = _duration * 0.5f;
        const f32 t = normalized_time(_elapsed, phase);
        const Rectf full = full_screen(r);
        const ShaderHandle sh = gpu ? r.builtin_shader(builtin_for(_kind)) : ShaderHandle{};
        if (gpu && sh && _snapshot.valid()) {
            const f32 aspect = full.h > 0.0f ? full.w / full.h : 1.0f;
            r.draw_shader_surface(full, sh, morph_params(_color, t, 0.0f, aspect),
                                  _snapshot.texture());
        } else {
            draw_fade(r, _color, t); // degrade: cover with color
        }
    }

private:
    std::unique_ptr<Scene> _next;
    TransitionKind _kind;
    f32 _duration;
    f32 _elapsed = 0.0f;
    Color _color;
    PooledTarget _snapshot;
    bool _captured = false;
};

} // namespace

FadeScene::FadeScene(FadeDirection direction, f32 duration, Color color, u8 max_alpha)
    : _direction(direction),
      _duration(std::max(duration, 0.001f)),
      _color(color),
      _max_alpha(max_alpha) {
}

void FadeScene::update(SceneContext& ctx) {
    _elapsed += std::max(ctx.dt, 0.0f);
    if (_elapsed >= _duration) {
        ctx.scenes.pop();
    }
}

void FadeScene::render(SceneContext& ctx) {
    const f32 t = normalized_time(_elapsed, _duration);
    const f32 alpha = _direction == FadeDirection::In ? (1.0f - t) : t;
    draw_fade(ctx.renderer, _color, alpha * (static_cast<f32>(_max_alpha) / 255.0f));
}

ReplaceWithFadeScene::ReplaceWithFadeScene(std::unique_ptr<Scene> next, f32 duration, Color color)
    : _next(std::move(next)),
      _duration(std::max(duration, 0.001f)),
      _color(color) {
}

void ReplaceWithFadeScene::update(SceneContext& ctx) {
    _elapsed += std::max(ctx.dt, 0.0f);
    if (_elapsed >= _duration) {
        ctx.scenes.pop();
        ctx.scenes.replace(std::move(_next));
        ctx.scenes.push(fade_in(_duration, _color));
    }
}

void ReplaceWithFadeScene::render(SceneContext& ctx) {
    draw_fade(ctx.renderer, _color, normalized_time(_elapsed, _duration));
}

std::unique_ptr<Scene> fade_in(f32 duration, Color color) {
    return std::make_unique<FadeScene>(FadeDirection::In, duration, color);
}

std::unique_ptr<Scene> fade_out(f32 duration, Color color) {
    return std::make_unique<FadeScene>(FadeDirection::Out, duration, color);
}

std::unique_ptr<Scene> replace_with_fade(std::unique_ptr<Scene> next, f32 duration, Color color) {
    return std::make_unique<ReplaceWithFadeScene>(std::move(next), duration, color);
}

std::unique_ptr<Scene> transition_to(std::unique_ptr<Scene> next, TransitionKind kind, f32 duration,
                                     Color color) {
    // Fade is the existing flat-color crossfade-through-color; the rest are shader morphs
    // (degrading to the same fade on backends without material support).
    if (kind == TransitionKind::Fade) {
        return replace_with_fade(std::move(next), duration, color);
    }
    return std::make_unique<TransitionOutScene>(std::move(next), kind, duration, color);
}

} // namespace kin
