#pragma once

#include <kin/renderer/color.hpp>
#include <kin/scene/scene.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string_view>

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
    // The scene below stays: `swap` changes it once the screen is covered.
    ReplaceWithFadeScene(std::function<void(SceneContext&)> swap,
                         f32 duration = default_fade_duration,
                         Color color = colors::black);

    std::string_view name() const override { return "ReplaceWithFadeScene"; }
    bool is_overlay() const override { return true; }
    bool updates_below() const override { return false; }

    void update(SceneContext& ctx) override;
    void render(SceneContext& ctx) override;

private:
    std::unique_ptr<Scene> _next;
    std::function<void(SceneContext&)> _swap;
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

// The same transitions over a scene that stays: `swap` changes it (reads
// another scene file into it, say) where transition_to would put the next
// scene in, the screen covered (a crossfade's: after the outgoing frame is
// kept). Pushed over the scene: ctx.scenes.push(transition_within(...)). The
// scene below does not update while it plays.
std::unique_ptr<Scene> transition_within(std::function<void(SceneContext&)> swap,
                                         TransitionKind kind,
                                         f32 duration = default_fade_duration,
                                         Color color = colors::black);

// "fade", "crossfade", "dissolve", "pixelate", "wipe", "iris" (as scene files
// and scripts name them), and back (none for another name).
std::string_view transition_name(TransitionKind kind);
std::optional<TransitionKind> transition_from_name(std::string_view name);

} // namespace kin
