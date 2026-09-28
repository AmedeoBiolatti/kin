#pragma once

#include <kin/renderer/color.hpp>
#include <kin/scene/scene.hpp>

#include <memory>

namespace kin {

// Default duration (seconds) for fade/transition scenes.
inline constexpr f32 default_fade_duration = 0.25f;

enum class FadeDirection {
    In,
    Out,
};

class FadeScene final : public Scene {
public:
    FadeScene(FadeDirection direction,
              f32 duration = default_fade_duration,
              Color color = colors::black,
              u8 max_alpha = 255);

    std::string_view name() const override { return "FadeScene"; }
    bool is_overlay() const override { return true; }
    bool updates_below() const override { return true; }

    void update(SceneContext& ctx) override;
    void render(SceneContext& ctx) override;

private:
    FadeDirection _direction = FadeDirection::In;
    f32 _duration = default_fade_duration;
    f32 _elapsed = 0.0f;
    Color _color = colors::black;
    u8 _max_alpha = 255;
};

class ReplaceWithFadeScene final : public Scene {
public:
    ReplaceWithFadeScene(std::unique_ptr<Scene> next,
                         f32 duration = default_fade_duration,
                         Color color = colors::black);

    std::string_view name() const override { return "ReplaceWithFadeScene"; }
    bool is_overlay() const override { return true; }
    bool updates_below() const override { return false; }

    void update(SceneContext& ctx) override;
    void render(SceneContext& ctx) override;

private:
    std::unique_ptr<Scene> _next;
    f32 _duration = default_fade_duration;
    f32 _elapsed = 0.0f;
    Color _color = colors::black;
};

std::unique_ptr<Scene> fade_in(f32 duration = default_fade_duration, Color color = colors::black);
std::unique_ptr<Scene> fade_out(f32 duration = default_fade_duration, Color color = colors::black);
std::unique_ptr<Scene> replace_with_fade(std::unique_ptr<Scene> next,
                                         f32 duration = default_fade_duration,
                                         Color color = colors::black);

// Shader-based scene transitions (GPU backend). Each replaces the current scene with
// `next` using a captured-snapshot morph (Dissolve/Pixelate/Wipe/Iris go scene->color->
// scene) or a true cross-dissolve (Crossfade blends the outgoing and incoming frames).
// On backends without material support they degrade to the flat-color fade. `Fade` is an
// alias for replace_with_fade, so transition_to is a drop-in superset of it.
enum class TransitionKind {
    Fade,
    Crossfade,
    Dissolve,
    Pixelate,
    Wipe,
    Iris,
};

std::unique_ptr<Scene> transition_to(std::unique_ptr<Scene> next,
                                     TransitionKind kind,
                                     f32 duration = default_fade_duration,
                                     Color color = colors::black);

} // namespace kin
