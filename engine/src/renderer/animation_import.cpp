#include <kin/renderer/animation_import.hpp>

#include <utility>
#include <vector>
#include <variant>

namespace kin {
namespace {

bool valid_source(Rectf source) {
    return source.w > 0.0f && source.h > 0.0f;
}

SpriteDefinition imported_sprite_definition(const SpriteCatalog& catalog,
                                            std::string sprite_id,
                                            const AnimationImportSet& import,
                                            const AnimationImportFrame& frame,
                                            const AnimationImportOptions& options) {
    if (!options.sprite_override_id.empty()) {
        const SpriteDefinition* overridden = catalog.sprite(options.sprite_override_id);
        if (!overridden) {
            return {};
        }
        SpriteDefinition copy = *overridden;
        copy.id = std::move(sprite_id);
        return copy;
    }

    Rectf source = frame.source;
    source.x += options.source_offset.x;
    source.y += options.source_offset.y;
    if (options.source_size.x > 0.0f && options.source_size.y > 0.0f) {
        source.w = options.source_size.x;
        source.h = options.source_size.y;
    }
    if (import.texture_id.empty() || !valid_source(source)) {
        return {};
    }

    return SpriteDefinition{
        .id = std::move(sprite_id),
        .texture_id = import.texture_id,
        .source = source,
        .offset = options.offset,
        .size = options.draw_size.x > 0.0f && options.draw_size.y > 0.0f ? options.draw_size : Vec2f{source.w, source.h},
        .pivot = frame.has_pivot ? frame.pivot : options.pivot,
    };
}

Clip make_import_clip(const AnimationImportClip& import_clip,
                      const AnimationImportOptions& options,
                      const std::vector<std::string>& sprite_ids,
                      bool reverse) {
    Clip clip;
    SpriteTrack sprites;

    const auto add_frame = [&](std::size_t index, f32& time) {
        const AnimationImportFrame& frame = import_clip.frames[index];
        sprites.keys.push_back(SpriteKey{
            .time = time,
            .sprite_id = sprite_ids[index],
            .pivot = frame.has_pivot ? frame.pivot : (import_clip.has_pivot ? import_clip.pivot : options.pivot),
            .has_pivot = frame.has_pivot || import_clip.has_pivot,
            .boxes = frame.boxes,
        });
        if (!frame.events.empty()) {
            EventTrack events;
            for (const AnimationEvent& event : frame.events) {
                events.keys.push_back(EventKey{.time = time, .event = event});
            }
            clip.events.push_back(std::move(events));
        }
        time += frame.duration;
    };

    f32 time = 0.0f;
    if (reverse) {
        for (std::size_t i = import_clip.frames.size(); i > 0; --i) {
            add_frame(i - 1, time);
        }
    } else {
        for (std::size_t i = 0; i < import_clip.frames.size(); ++i) {
            add_frame(i, time);
        }
    }

    clip.duration = time;
    if (!sprites.keys.empty()) {
        clip.sprites.push_back(std::move(sprites));
    }
    return clip;
}

Animation make_import_animation(const AnimationImportClip& import_clip,
                                const AnimationImportOptions& options,
                                const std::vector<std::string>& sprite_ids) {
    Animation animation{.name = import_clip.name};
    Clip forward = make_import_clip(import_clip, options, sprite_ids, false);

    switch (import_clip.playback) {
    case AnimationImportPlayback::Once:
        animation.root = clip_node(std::move(forward));
        break;
    case AnimationImportPlayback::Loop:
        animation.root = repeat_node(clip_node(std::move(forward)), 0);
        break;
    case AnimationImportPlayback::PingPong:
        if (import_clip.frames.size() <= 1) {
            animation.root = repeat_node(clip_node(std::move(forward)), 0);
        } else {
            AnimationImportClip reverse_import = import_clip;
            reverse_import.frames.clear();
            std::vector<std::string> reverse_ids;
            for (std::size_t i = import_clip.frames.size() - 1; i-- > 0;) {
                reverse_import.frames.push_back(import_clip.frames[i]);
                reverse_ids.push_back(sprite_ids[i]);
            }
            Clip reverse = make_import_clip(reverse_import, options, reverse_ids, false);
            std::vector<AnimationNode> ping_pong;
            ping_pong.push_back(clip_node(std::move(forward)));
            ping_pong.push_back(clip_node(std::move(reverse)));
            animation.root = repeat_node(sequence_node(std::move(ping_pong)), 0);
        }
        break;
    }

    return animation;
}

} // namespace

std::string animation_import_sprite_id(std::string_view id_prefix,
                                       std::string_view clip_name,
                                       i32 frame_index) {
    std::string id;
    if (!id_prefix.empty()) {
        id += id_prefix;
        id += ".";
    }
    id += clip_name;
    id += ".";
    id += std::to_string(frame_index);
    return id;
}

bool add_animation_sprites(SpriteCatalog& catalog,
                           const AnimationImportSet& import,
                           AnimationImportOptions options,
                           AnimationRegistry* out_animations) {
    if (!import.texture_id.empty() && !catalog.texture(import.texture_id)) {
        catalog.set_texture(import.texture_id, import.texture_path, {});
    }

    bool ok = true;
    std::vector<Animation> generated;
    for (const AnimationImportClip& import_clip : import.clips) {
        if (import_clip.name.empty() || import_clip.frames.empty()) {
            ok = false;
            continue;
        }

        bool clip_ok = true;
        std::vector<std::string> sprite_ids;
        sprite_ids.reserve(import_clip.frames.size());

        for (std::size_t i = 0; i < import_clip.frames.size(); ++i) {
            const AnimationImportFrame& frame = import_clip.frames[i];
            if (frame.duration <= 0.0f) {
                ok = false;
                clip_ok = false;
                continue;
            }
            const std::string sprite_id = animation_import_sprite_id(options.id_prefix, import_clip.name, static_cast<i32>(i));
            SpriteDefinition sprite = imported_sprite_definition(catalog, sprite_id, import, frame, options);
            if (sprite.id.empty()) {
                ok = false;
                clip_ok = false;
                continue;
            }
            catalog.add(sprite);
            sprite_ids.push_back(std::move(sprite.id));
        }

        if (clip_ok && sprite_ids.size() == import_clip.frames.size()) {
            Animation animation = make_import_animation(import_clip, options, sprite_ids);
            std::string error;
            if (validate(animation, error)) {
                generated.push_back(std::move(animation));
            } else {
                ok = false;
            }
        } else {
            ok = false;
        }
    }

    if (out_animations) {
        for (Animation& animation : generated) {
            out_animations->add(std::move(animation));
        }
    }
    return ok;
}

bool add_animation_sprites(SpriteCatalog& catalog,
                           AssetManager& assets,
                           Renderer2D& renderer,
                           const AnimationImportSet& import,
                           AnimationImportOptions options,
                           AnimationRegistry* out_animations) {
    if (!import.texture_id.empty() && !import.texture_path.empty() && !catalog.texture(import.texture_id)) {
        const std::shared_ptr<const Image> image = assets.load<Image>(import.texture_path);
        catalog.set_texture(import.texture_id, import.texture_path, renderer.create_texture_from_rgba(image->rgba.data(), image->size));
    }
    return add_animation_sprites(catalog, import, std::move(options), out_animations);
}

} // namespace kin
