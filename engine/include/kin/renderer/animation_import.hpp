#pragma once

#include <kin/anim/animation.hpp>
#include <kin/anim/events.hpp>
#include <kin/anim/registry.hpp>
#include <kin/anim/track.hpp>
#include <kin/renderer/sprite_catalog.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace kin {

enum class AnimationImportPlayback : u8 {
    Loop,
    Once,
    PingPong,
};

struct AnimationImportFrame {
    Rectf source{};
    f32 duration = 0.15f;
    Vec2f pivot{};
    bool has_pivot = false;
    std::vector<AnimationEvent> events;
    std::vector<SpriteBox> boxes;
};

struct AnimationImportClip {
    std::string name;
    std::vector<AnimationImportFrame> frames;
    AnimationImportPlayback playback = AnimationImportPlayback::Loop;
    Vec2f pivot{};
    bool has_pivot = false;
};

struct AnimationImportSet {
    std::string texture_id;
    std::string texture_path;
    std::vector<AnimationImportClip> clips;
};

struct AnimationImportOptions {
    std::string id_prefix;
    std::string sprite_override_id;
    Vec2f source_offset{};
    Vec2f source_size{};
    Vec2f draw_size{};
    Vec2f pivot{0.5f, 0.5f};
    Vec2f offset{};
};

std::string animation_import_sprite_id(std::string_view id_prefix,
                                       std::string_view clip_name,
                                       i32 frame_index);

bool add_animation_sprites(SpriteCatalog& catalog,
                           const AnimationImportSet& import,
                           AnimationImportOptions options = {},
                           AnimationRegistry* out_animations = nullptr);

bool add_animation_sprites(SpriteCatalog& catalog,
                           AssetManager& assets,
                           Renderer2D& renderer,
                           const AnimationImportSet& import,
                           AnimationImportOptions options = {},
                           AnimationRegistry* out_animations = nullptr);

} // namespace kin
