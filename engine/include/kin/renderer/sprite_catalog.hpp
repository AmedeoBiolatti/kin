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

    SpriteRef ref(std::string_view id) const;
    bool contains(std::string_view id) const;
    bool resolve(std::string_view id, ResolvedSprite& out) const;

    const TextureAssetRef* texture(std::string_view id) const;
    const SpriteSheetDefinition* sheet(std::string_view id) const;
    const SpriteDefinition* sprite(std::string_view id) const;
    std::vector<std::string> sprite_ids() const;
    const std::unordered_map<std::string, TextureAssetRef>& textures() const { return _textures; }
    const std::unordered_map<std::string, SpriteSheetDefinition>& sheets() const { return _sheets; }
    const std::unordered_map<std::string, SpriteDefinition>& sprites() const { return _sprites; }

private:
    std::unordered_map<std::string, TextureAssetRef> _textures;
    std::unordered_map<std::string, SpriteSheetDefinition> _sheets;
    std::unordered_map<std::string, SpriteDefinition> _sprites;
};

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
