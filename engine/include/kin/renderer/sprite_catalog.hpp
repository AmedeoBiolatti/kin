#pragma once

#include <kin/assets/asset_manager.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <kin/renderer/sprite.hpp>
#include <kin/renderer/sprite_sheet.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace kin {

class SpriteCatalog;

struct SpriteRef {
    const SpriteCatalog* catalog = nullptr;
    std::string id;

    bool valid() const;
    explicit operator bool() const { return valid(); }
};

struct TextureAssetRef {
    std::string id;
    std::string path;
    Texture texture;
};

struct SpriteSheetDefinition {
    std::string id;
    std::string texture_id;
    SpriteSheetGrid grid{};
};

struct SpriteDefinition {
    std::string id;
    std::string texture_id;
    std::string sheet_id;
    i32 frame = -1;
    Rectf source{};
    Vec2f offset{};
    Vec2f size{};
    Vec2f pivot{};
};

// How an animation goes on once at its last frame: from its first again
// (Loop); not (Once: its last stays); back to its first, and so on (PingPong).
enum class SpritePlayback : u8 {
    Loop,
    Once,
    PingPong,
};

struct SpriteAnimationFrame {
    std::string sprite; // a sprite of the catalog
    f32 hold = 1.0f;    // how long it shows, in frames (of 1/fps seconds)
};

// An animation of a catalog's sprites: its frames shown in turn, `fps` a
// second (one held longer for its `hold`). SpriteAnimator plays one on an
// entity (kin/ecs/sprite_animator.hpp).
struct SpriteAnimationDefinition {
    std::string id;
    std::vector<SpriteAnimationFrame> frames;
    f32 fps = 10.0f;
    SpritePlayback playback = SpritePlayback::Loop;
};

// How long it plays before it starts again (a PingPong one: there and back),
// in seconds; 0 without frames.
f32 sprite_animation_length(const SpriteAnimationDefinition& animation);
// The frame (its index) shown `time` seconds into it: a Once one stays on
// its last, and says it `finished` (when asked). -1 when it has no frames.
i32 sprite_animation_frame(const SpriteAnimationDefinition& animation, f32 time, bool* finished = nullptr);

struct ResolvedSprite {
    Sprite sprite;
    Vec2f offset{};
    Vec2f size{};
    Vec2f pivot{};

    bool valid() const { return sprite.valid(); }
};

class SpriteCatalog {
public:
    void clear();

    void set_texture(std::string_view id, Texture texture);
    void set_texture(std::string_view id, std::string_view path, Texture texture);
    void add_sheet(SpriteSheetDefinition sheet);
    void add(SpriteDefinition sprite);
    void add_sheet_sprite(std::string_view id,
                          std::string_view sheet_id,
                          i32 frame,
                          Vec2f size = {},
                          Vec2f pivot = {},
                          Vec2f offset = {});
    void add_animation(SpriteAnimationDefinition animation);
    // Gone: false when there was none of that id. A sprite removed leaves the
    // animations showing it as they are (resolve() fails for it).
    bool remove_texture(std::string_view id);
    bool remove_sheet(std::string_view id);
    bool remove(std::string_view sprite_id);
    bool remove_animation(std::string_view id);

    SpriteRef ref(std::string_view id) const;
    bool contains(std::string_view id) const;
    bool resolve(std::string_view id, ResolvedSprite& out) const;

    const TextureAssetRef* texture(std::string_view id) const;
    const SpriteSheetDefinition* sheet(std::string_view id) const;
    const SpriteDefinition* sprite(std::string_view id) const;
    const SpriteAnimationDefinition* animation(std::string_view id) const;
    std::vector<std::string> sprite_ids() const;
    std::vector<std::string> animation_ids() const;
    const std::unordered_map<std::string, TextureAssetRef>& textures() const { return _textures; }
    const std::unordered_map<std::string, SpriteSheetDefinition>& sheets() const { return _sheets; }
    const std::unordered_map<std::string, SpriteDefinition>& sprites() const { return _sprites; }
    const std::unordered_map<std::string, SpriteAnimationDefinition>& animations() const { return _animations; }

private:
    std::unordered_map<std::string, TextureAssetRef> _textures;
    std::unordered_map<std::string, SpriteSheetDefinition> _sheets;
    std::unordered_map<std::string, SpriteDefinition> _sprites;
    std::unordered_map<std::string, SpriteAnimationDefinition> _animations;
};

// A catalog from the text of a .kinsprites file (textures not made): false,
// and why in `error` ("line 3: malformed sheet"), when it does not read. One
// with no sprites reads (load_sprite_catalog() refuses it).
bool parse_sprite_catalog(std::string_view text, SpriteCatalog& out, std::string& error);
// Its text, as save_sprite_catalog() writes it (each kind in its ids' order).
std::string serialize_sprite_catalog(const SpriteCatalog& catalog);

SpriteCatalog load_sprite_catalog(const std::filesystem::path& path);
SpriteCatalog load_sprite_catalog(AssetManager& assets, Renderer2D& renderer, const std::filesystem::path& path);
bool save_sprite_catalog(const SpriteCatalog& catalog, const std::filesystem::path& path);
void register_sprite_catalog_loader(AssetManager& assets, Renderer2D& renderer);

class AssetServer;

// Registers a dependency-aware async loader for `.kinsprites` catalogs: it
// parses the catalog (without a renderer) and declares each referenced texture
// image as a dependency, so the catalog becomes `ready()` only once its texture
// images have loaded. GPU texture upload remains a separate concern.
void register_sprite_catalog_async_loader(AssetServer& server);

} // namespace kin
