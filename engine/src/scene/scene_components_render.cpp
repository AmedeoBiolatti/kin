#include "scene_components_internal.hpp"
#include "scene_components_blend.hpp"

#include <kin/assets/asset_manager.hpp>
#include <kin/ecs/render.hpp>
#include <kin/ecs/sprite_animator.hpp>
#if KIN_ENABLE_AUDIO
#include <kin/ecs/audio.hpp>
#endif

#include <optional>
#include <string>

namespace kin::scene_detail {

void register_scene_render_components(EcsWorld& world) {
    if (!world.components().find("Transform2D")) {
        world.components().native<Transform2D>("Transform2D")
            .field("pos", &Transform2D::pos)
            .field("rotation", &Transform2D::rotation)
            .field("scale", &Transform2D::scale);
    }
    if (!world.components().find("RectRenderer")) {
        auto rect = world.components().native<RectRenderer>("RectRenderer");
        rect.field("offset", &RectRenderer::offset)
            .field("size", &RectRenderer::size)
            .field("color", &RectRenderer::color)
            .field("layer", &RectRenderer::layer)
            .field("order", &RectRenderer::order)
            .field("visible", &RectRenderer::visible);
        describe_blend(rect);
    }
    if (!world.components().find("TextureRenderer")) {
        // An image of the game's content, by its path (Assets2D binds it).
        auto texture = world.components().native<TextureRenderer>("TextureRenderer");
        texture.field("asset", &TextureRenderer::asset)
            .asset_type(std::string{asset_type_name(AssetType::Image)})
            .field("offset", &TextureRenderer::offset)
            .field("size", &TextureRenderer::size)
            .field("pivot", &TextureRenderer::pivot)
            .field("tint", &TextureRenderer::tint)
            .field("layer", &TextureRenderer::layer)
            .field("order", &TextureRenderer::order)
            .field("visible", &TextureRenderer::visible)
            .field("flip_x", &TextureRenderer::flip_x)
            .field("flip_y", &TextureRenderer::flip_y);
        describe_blend(texture);
    }
    if (!world.components().find("SpriteRenderer")) {
        // A sprite of a catalog of the game's content, by the catalog's path and
        // the sprite's id (Assets2D binds the catalog).
        auto sprite = world.components().native<SpriteRenderer>("SpriteRenderer");
        sprite.field("asset", &SpriteRenderer::asset)
            .asset_type(std::string{asset_type_name(AssetType::SpriteCatalog)})
            .property<std::string>("sprite", [](const SpriteRenderer& r) { return r.sprite.id; },
                                   [](SpriteRenderer& r, const std::string& id) {
                                       r.sprite.id = id;
                                       return true;
                                   })
            .entry_of("asset")
            .field("offset", &SpriteRenderer::offset)
            .field("size", &SpriteRenderer::size)
            .description("Zero: the sprite's own size.")
            .field("pivot", &SpriteRenderer::pivot)
            .description("A fraction of the sprite; below zero: the catalog's pivot.")
            .field("tint", &SpriteRenderer::tint)
            .field("rotation", &SpriteRenderer::rotation)
            .units("degrees")
            .field("layer", &SpriteRenderer::layer)
            .field("order", &SpriteRenderer::order)
            .field("y_sort", &SpriteRenderer::y_sort)
            .field("visible", &SpriteRenderer::visible)
            .field("flip_x", &SpriteRenderer::flip_x)
            .field("flip_y", &SpriteRenderer::flip_y);
        describe_blend(sprite);
    }
    if (!world.components().find("SpriteAnimator")) {
        // An animation of its SpriteRenderer's catalog played on it
        // (update_sprite_animators sets the renderer's sprite).
        world.components().native<SpriteAnimator>("SpriteAnimator")
            .field("animation", &SpriteAnimator::animation)
            .entry_of("SpriteRenderer.asset")
            .description("An animation of its SpriteRenderer's catalog.")
            .field("speed", &SpriteAnimator::speed)
            .range(0.0, 100.0)
            .description("1: as the animation says; 2: twice as fast.")
            .field("playing", &SpriteAnimator::playing)
            .property<i32>("frame", [](const SpriteAnimator& a) { return a.frame; },
                           [](SpriteAnimator&, const i32&) { return true; }) // as it is: nothing to set
            .readonly()
            .description("The frame of the animation shown now (-1: none).")
            .property<bool>("finished", [](const SpriteAnimator& a) { return a.finished; },
                            [](SpriteAnimator&, const bool&) { return true; })
            .readonly()
            .description("A Once animation has played through.");
    }
#if KIN_ENABLE_AUDIO
    if (!world.components().find("AudioEmitter")) {
        // A sound of the game's content (a file, or a cue of an audio catalog)
        // playing from the entity (Assets2D binds it; update_audio_emitters plays it).
        world.components().native<AudioEmitter>("AudioEmitter")
            .field("asset", &AudioEmitter::asset)
            .asset_type(std::string{asset_type_name(AssetType::Audio)} + "," + std::string{asset_type_name(AssetType::AudioCatalog)})
            .field("cue", &AudioEmitter::cue)
            .entry_of("asset")
            .description("A cue of the catalog; empty for a sound file.")
            .field("auto_start", &AudioEmitter::auto_start)
            .field("volume", &AudioEmitter::volume)
            .range(0.0, 4.0)
            .field("pitch", &AudioEmitter::pitch)
            .range(0.01, 8.0)
            .field("paused", &AudioEmitter::paused)
            .field("fade_in", &AudioEmitter::fade_in)
            .units("seconds")
            .property<std::string>("when_done",
                                   [](const AudioEmitter& e) {
                                       switch (e.when_done) {
                                       case AudioEmitterEnd::Restart: return std::string("restart");
                                       case AudioEmitterEnd::Keep: return std::string("keep");
                                       case AudioEmitterEnd::Remove: return std::string("remove");
                                       case AudioEmitterEnd::Despawn: return std::string("despawn");
                                       }
                                       return std::string("restart");
                                   },
                                   [](AudioEmitter& e, const std::string& value) {
                                       if (value == "restart") e.when_done = AudioEmitterEnd::Restart;
                                       else if (value == "keep") e.when_done = AudioEmitterEnd::Keep;
                                       else if (value == "remove") e.when_done = AudioEmitterEnd::Remove;
                                       else if (value == "despawn") e.when_done = AudioEmitterEnd::Despawn;
                                       else return false;
                                       return true;
                                   })
            .choices({"restart", "keep", "remove", "despawn"})
            .description("What it does once its sound ends.")
            .field("offset_3d", &AudioEmitter::offset_3d);
    }
#endif
    if (!world.components().find("LineRenderer")) {
        world.components().native<LineRenderer>("LineRenderer")
            .field("a", &LineRenderer::a)
            .field("b", &LineRenderer::b)
            .field("color", &LineRenderer::color)
            .field("layer", &LineRenderer::layer)
            .field("order", &LineRenderer::order)
            .field("visible", &LineRenderer::visible);
    }
}

} // namespace kin::scene_detail
