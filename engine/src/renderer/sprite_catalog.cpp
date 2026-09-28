#include <kin/renderer/sprite_catalog.hpp>

#include <kin/assets/asset_server.hpp>
#include <kin/platform/log.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace kin {
namespace {

Rectf sheet_frame_source(const Texture& texture, const SpriteSheetGrid& grid, i32 frame) {
    if (!texture.valid() || grid.tile_w <= 0 || grid.tile_h <= 0 || frame < 0) {
        return {};
    }

    const i32 available_w = texture.size().x - grid.margin * 2 + grid.spacing;
    const i32 cols = available_w > 0 ? available_w / (grid.tile_w + grid.spacing) : 0;
    if (cols <= 0) {
        return {};
    }

    const i32 col = frame % cols;
    const i32 row = frame / cols;
    return {
        static_cast<f32>(grid.margin + col * (grid.tile_w + grid.spacing)),
        static_cast<f32>(grid.margin + row * (grid.tile_h + grid.spacing)),
        static_cast<f32>(grid.tile_w),
        static_cast<f32>(grid.tile_h),
    };
}

std::string trim_comment(std::string line) {
    if (const auto comment = line.find('#'); comment != std::string::npos) {
        line.erase(comment);
    }
    return line;
}

void warn_empty_direct_sprite(const std::filesystem::path& path, i32 line_no, const SpriteDefinition& sprite_def) {
    if (sprite_def.source.w > 0.0f && sprite_def.source.h > 0.0f) {
        return;
    }

    KIN_LOG_WARN_F("asset",
                   "sprite catalog contains empty direct sprite",
                   (LogFields{
                       {.name = "path", .value = path.string()},
                       {.name = "type", .value = "SpriteCatalog"},
                       {.name = "line", .value = std::to_string(line_no)},
                       {.name = "sprite", .value = sprite_def.id},
                       {.name = "texture", .value = sprite_def.texture_id},
                       {.name = "source_w", .value = std::to_string(sprite_def.source.w)},
                       {.name = "source_h", .value = std::to_string(sprite_def.source.h)},
                   }));
}

void warn_direct_sprite_outside_texture(const std::filesystem::path& path,
                                        const SpriteDefinition& sprite_def,
                                        const TextureAssetRef& texture_ref) {
    if (!sprite_def.sheet_id.empty() || !texture_ref.texture.valid()) {
        return;
    }

    const Vec2i texture_size = texture_ref.texture.size();
    if (sprite_def.source.x >= 0.0f && sprite_def.source.y >= 0.0f &&
        sprite_def.source.x + sprite_def.source.w <= static_cast<f32>(texture_size.x) &&
        sprite_def.source.y + sprite_def.source.h <= static_cast<f32>(texture_size.y)) {
        return;
    }

    KIN_LOG_WARN_F("asset",
                   "sprite catalog direct sprite source is outside texture",
                   (LogFields{
                       {.name = "path", .value = path.string()},
                       {.name = "type", .value = "SpriteCatalog"},
                       {.name = "sprite", .value = sprite_def.id},
                       {.name = "texture", .value = sprite_def.texture_id},
                       {.name = "texture_w", .value = std::to_string(texture_size.x)},
                       {.name = "texture_h", .value = std::to_string(texture_size.y)},
                       {.name = "source_x", .value = std::to_string(sprite_def.source.x)},
                       {.name = "source_y", .value = std::to_string(sprite_def.source.y)},
                       {.name = "source_w", .value = std::to_string(sprite_def.source.w)},
                       {.name = "source_h", .value = std::to_string(sprite_def.source.h)},
                   }));
}

} // namespace

bool SpriteRef::valid() const {
    return catalog && !id.empty() && catalog->contains(id);
}

void SpriteCatalog::clear() {
    _textures.clear();
    _sheets.clear();
    _sprites.clear();
}

void SpriteCatalog::set_texture(std::string_view id, Texture texture) {
    set_texture(id, {}, std::move(texture));
}

void SpriteCatalog::set_texture(std::string_view id, std::string_view path, Texture texture) {
    const std::string key{id};
    _textures[key] = TextureAssetRef{
        .id = key,
        .path = std::string{path},
        .texture = std::move(texture),
    };
}

void SpriteCatalog::add_sheet(SpriteSheetDefinition sheet_def) {
    _sheets[sheet_def.id] = std::move(sheet_def);
}

void SpriteCatalog::add(SpriteDefinition sprite_def) {
    _sprites[sprite_def.id] = std::move(sprite_def);
}

void SpriteCatalog::add_sheet_sprite(std::string_view id,
                                     std::string_view sheet_id,
                                     i32 frame,
                                     Vec2f size,
                                     Vec2f pivot,
                                     Vec2f offset) {
    add(SpriteDefinition{
        .id = std::string{id},
        .sheet_id = std::string{sheet_id},
        .frame = frame,
        .offset = offset,
        .size = size,
        .pivot = pivot,
    });
}

SpriteRef SpriteCatalog::ref(std::string_view id) const {
    return {
        .catalog = this,
        .id = std::string{id},
    };
}

bool SpriteCatalog::contains(std::string_view id) const {
    return _sprites.contains(std::string{id});
}

bool SpriteCatalog::resolve(std::string_view id, ResolvedSprite& out) const {
    const SpriteDefinition* sprite_def = sprite(id);
    if (!sprite_def) {
        return false;
    }

    std::string texture_id = sprite_def->texture_id;
    Rectf source = sprite_def->source;
    if (!sprite_def->sheet_id.empty()) {
        const SpriteSheetDefinition* sheet_def = sheet(sprite_def->sheet_id);
        if (!sheet_def) {
            return false;
        }
        texture_id = sheet_def->texture_id;
        const TextureAssetRef* texture_ref = texture(texture_id);
        if (!texture_ref) {
            return false;
        }
        source = sheet_frame_source(texture_ref->texture, sheet_def->grid, sprite_def->frame);
    }

    const TextureAssetRef* texture_ref = texture(texture_id);
    if (!texture_ref || !texture_ref->texture.valid() || source.w <= 0.0f || source.h <= 0.0f) {
        return false;
    }

    out = ResolvedSprite{
        .sprite = {
            .texture = texture_ref->texture,
            .source = source,
        },
        .offset = sprite_def->offset,
        .size = sprite_def->size.x > 0.0f && sprite_def->size.y > 0.0f ? sprite_def->size : Vec2f{source.w, source.h},
        .pivot = sprite_def->pivot,
    };
    return true;
}

const TextureAssetRef* SpriteCatalog::texture(std::string_view id) const {
    const auto found = _textures.find(std::string{id});
    return found == _textures.end() ? nullptr : &found->second;
}

const SpriteSheetDefinition* SpriteCatalog::sheet(std::string_view id) const {
    const auto found = _sheets.find(std::string{id});
    return found == _sheets.end() ? nullptr : &found->second;
}

const SpriteDefinition* SpriteCatalog::sprite(std::string_view id) const {
    const auto found = _sprites.find(std::string{id});
    return found == _sprites.end() ? nullptr : &found->second;
}

std::vector<std::string> SpriteCatalog::sprite_ids() const {
    std::vector<std::string> ids;
    ids.reserve(_sprites.size());
    for (const auto& [id, sprite_def] : _sprites) {
        ids.push_back(id);
    }
    std::ranges::sort(ids);
    return ids;
}

SpriteCatalog load_sprite_catalog(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) {
        KIN_LOG_ERROR_F("asset",
                        "sprite catalog open failed",
                        (LogFields{
                            {.name = "path", .value = path.string()},
                            {.name = "type", .value = "SpriteCatalog"},
                        }));
        throw std::runtime_error("Failed to open sprite catalog: " + path.string());
    }

    SpriteCatalog catalog;
    std::string line;
    i32 line_no = 0;
    const auto fail = [&](std::string_view message) {
        throw std::runtime_error(path.string() + ":" + std::to_string(line_no) + ": " + std::string{message});
    };

    while (std::getline(file, line)) {
        ++line_no;
        std::istringstream in(trim_comment(std::move(line)));
        std::string kind;
        if (!(in >> kind)) {
            continue;
        }

        if (kind == "texture") {
            std::string id;
            std::string rel_path;
            if (!(in >> id >> rel_path)) {
                fail("malformed texture");
            }
            catalog.set_texture(id, rel_path, {});
            continue;
        }

        if (kind == "sheet") {
            SpriteSheetDefinition sheet_def;
            if (!(in >> sheet_def.id >> sheet_def.texture_id >> sheet_def.grid.tile_w >> sheet_def.grid.tile_h)) {
                fail("malformed sheet");
            }
            in >> sheet_def.grid.spacing >> sheet_def.grid.margin;
            catalog.add_sheet(std::move(sheet_def));
            continue;
        }

        if (kind == "sprite") {
            SpriteDefinition sprite_def;
            if (!(in >> sprite_def.id >> sprite_def.texture_id
                     >> sprite_def.source.x >> sprite_def.source.y >> sprite_def.source.w >> sprite_def.source.h)) {
                fail("malformed sprite");
            }
            sprite_def.size = {sprite_def.source.w, sprite_def.source.h};
            if (!(in >> sprite_def.size.x >> sprite_def.size.y)) {
                sprite_def.size = {sprite_def.source.w, sprite_def.source.h};
            }
            if (!(in >> sprite_def.pivot.x >> sprite_def.pivot.y)) {
                sprite_def.pivot = {0.5f, 0.5f};
            }
            if (!(in >> sprite_def.offset.x >> sprite_def.offset.y)) {
                sprite_def.offset = {};
            }
            warn_empty_direct_sprite(path, line_no, sprite_def);
            catalog.add(std::move(sprite_def));
            continue;
        }

        if (kind == "sheet_sprite") {
            SpriteDefinition sprite_def;
            if (!(in >> sprite_def.id >> sprite_def.sheet_id >> sprite_def.frame)) {
                fail("malformed sheet_sprite");
            }
            if (!(in >> sprite_def.size.x >> sprite_def.size.y)) {
                sprite_def.size = {};
            }
            if (!(in >> sprite_def.pivot.x >> sprite_def.pivot.y)) {
                sprite_def.pivot = {0.5f, 0.5f};
            }
            if (!(in >> sprite_def.offset.x >> sprite_def.offset.y)) {
                sprite_def.offset = {};
            }
            catalog.add(std::move(sprite_def));
            continue;
        }

        fail("unknown directive");
    }

    if (catalog.sprite_ids().empty()) {
        KIN_LOG_ERROR_F("asset",
                        "sprite catalog invalid",
                        (LogFields{
                            {.name = "path", .value = path.string()},
                            {.name = "type", .value = "SpriteCatalog"},
                            {.name = "error", .value = "no sprites"},
                        }));
        throw std::runtime_error("Sprite catalog has no sprites: " + path.string());
    }
    KIN_LOG_INFO_F("asset",
                   "sprite catalog loaded",
                   (LogFields{
                       {.name = "path", .value = path.string()},
                       {.name = "type", .value = "SpriteCatalog"},
                       {.name = "sprites", .value = std::to_string(catalog.sprite_ids().size())},
                       {.name = "textures", .value = std::to_string(catalog.textures().size())},
                   }));
    return catalog;
}

SpriteCatalog load_sprite_catalog(AssetManager& assets, Renderer2D& renderer, const std::filesystem::path& path) {
    SpriteCatalog catalog = load_sprite_catalog(path);
    std::vector<TextureAssetRef> textures;
    textures.reserve(catalog.textures().size());
    for (const auto& [id, texture] : catalog.textures()) {
        textures.push_back(texture);
    }
    for (const TextureAssetRef& texture : textures) {
        if (texture.path.empty()) {
            continue;
        }
        const std::shared_ptr<const Image> image = assets.load<Image>(texture.path);
        catalog.set_texture(texture.id, texture.path, renderer.create_texture_from_rgba(image->rgba.data(), image->size));
    }
    for (const auto& [id, sprite_def] : catalog.sprites()) {
        (void)id;
        if (!sprite_def.sheet_id.empty()) {
            continue;
        }
        const TextureAssetRef* texture_ref = catalog.texture(sprite_def.texture_id);
        if (texture_ref) {
            warn_direct_sprite_outside_texture(path, sprite_def, *texture_ref);
        }
    }
    return catalog;
}

bool save_sprite_catalog(const SpriteCatalog& catalog, const std::filesystem::path& path) {
    std::ofstream out(path);
    if (!out) {
        return false;
    }

    std::vector<const TextureAssetRef*> textures;
    textures.reserve(catalog.textures().size());
    for (const auto& [id, texture] : catalog.textures()) {
        textures.push_back(&texture);
    }
    std::ranges::sort(textures, [](const TextureAssetRef* a, const TextureAssetRef* b) {
        return a->id < b->id;
    });

    std::vector<const SpriteSheetDefinition*> sheets;
    sheets.reserve(catalog.sheets().size());
    for (const auto& [id, sheet_def] : catalog.sheets()) {
        sheets.push_back(&sheet_def);
    }
    std::ranges::sort(sheets, [](const SpriteSheetDefinition* a, const SpriteSheetDefinition* b) {
        return a->id < b->id;
    });

    std::vector<const SpriteDefinition*> sprites;
    sprites.reserve(catalog.sprites().size());
    for (const auto& [id, sprite_def] : catalog.sprites()) {
        sprites.push_back(&sprite_def);
    }
    std::ranges::sort(sprites, [](const SpriteDefinition* a, const SpriteDefinition* b) {
        return a->id < b->id;
    });

    out << "# kin sprite catalog\n";
    for (const TextureAssetRef* texture : textures) {
        if (texture->path.empty()) {
            return false;
        }
        out << "texture " << texture->id << ' ' << texture->path << '\n';
    }
    if (!textures.empty() && (!sheets.empty() || !sprites.empty())) {
        out << '\n';
    }
    for (const SpriteSheetDefinition* sheet_def : sheets) {
        out << "sheet " << sheet_def->id << ' ' << sheet_def->texture_id
            << ' ' << sheet_def->grid.tile_w << ' ' << sheet_def->grid.tile_h
            << ' ' << sheet_def->grid.spacing << ' ' << sheet_def->grid.margin << '\n';
    }
    if (!sheets.empty() && !sprites.empty()) {
        out << '\n';
    }
    for (const SpriteDefinition* sprite_def : sprites) {
        if (!sprite_def->sheet_id.empty()) {
            out << "sheet_sprite " << sprite_def->id << ' ' << sprite_def->sheet_id << ' ' << sprite_def->frame
                << ' ' << sprite_def->size.x << ' ' << sprite_def->size.y
                << ' ' << sprite_def->pivot.x << ' ' << sprite_def->pivot.y
                << ' ' << sprite_def->offset.x << ' ' << sprite_def->offset.y << '\n';
            continue;
        }
        out << "sprite " << sprite_def->id << ' ' << sprite_def->texture_id
            << ' ' << sprite_def->source.x << ' ' << sprite_def->source.y << ' ' << sprite_def->source.w << ' ' << sprite_def->source.h
            << ' ' << sprite_def->size.x << ' ' << sprite_def->size.y
            << ' ' << sprite_def->pivot.x << ' ' << sprite_def->pivot.y
            << ' ' << sprite_def->offset.x << ' ' << sprite_def->offset.y << '\n';
    }

    return static_cast<bool>(out);
}

void register_sprite_catalog_loader(AssetManager& assets, Renderer2D& renderer) {
    assets.register_loader<SpriteCatalog>([&assets, &renderer](const std::filesystem::path& path) {
        return load_sprite_catalog(assets, renderer, path);
    });
}

void register_sprite_catalog_async_loader(AssetServer& server) {
    server.register_async_loader<SpriteCatalog>(
        [](const std::filesystem::path& path, LoadContext& ctx) {
            SpriteCatalog catalog = load_sprite_catalog(path);
            for (const auto& [id, texture] : catalog.textures()) {
                if (!texture.path.empty()) {
                    ctx.require<Image>(texture.path);
                }
            }
            return catalog;
        });
}

} // namespace kin
