#include <kin/renderer/sprite_catalog.hpp>

#include <kin/assets/asset_server.hpp>
#include <kin/assets/content.hpp>
#include <kin/platform/log.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <type_traits>
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

std::string_view playback_text(SpritePlayback playback) {
    switch (playback) {
    case SpritePlayback::Loop: return "loop";
    case SpritePlayback::Once: return "once";
    case SpritePlayback::PingPong: return "pingpong";
    }
    return "loop";
}

bool parse_playback(std::string_view text, SpritePlayback& out) {
    for (const SpritePlayback playback : {SpritePlayback::Loop, SpritePlayback::Once, SpritePlayback::PingPong}) {
        if (text == playback_text(playback)) {
            out = playback;
            return true;
        }
    }
    return false;
}

// "hero.walk.2" or, held for longer, "hero.walk.2*2".
bool parse_animation_frame(const std::string& token, SpriteAnimationFrame& out) {
    out = {.sprite = token, .hold = 1.0f};
    if (const auto star = token.rfind('*'); star != std::string::npos) {
        std::istringstream in(token.substr(star + 1));
        f32 hold = 0.0f;
        if (!(in >> hold) || !in.eof() || !(hold > 0.0f) || star == 0) {
            return false;
        }
        out = {.sprite = token.substr(0, star), .hold = hold};
    }
    return !out.sprite.empty();
}

// The directives of a catalog's text into `catalog`: false at the first that
// does not read (its line and why in `error`). `path` (when given) names the
// file in the warnings logged for what reads but shows nothing.
bool read_catalog(std::string_view text, const std::filesystem::path* path, SpriteCatalog& catalog, std::string& error) {
    std::istringstream file{std::string{text}};
    std::string line;
    i32 line_no = 0;
    const auto fail = [&](std::string_view message) {
        error = "line " + std::to_string(line_no) + ": " + std::string{message};
        return false;
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
                return fail("malformed texture");
            }
            catalog.set_texture(id, rel_path, {});
            continue;
        }

        if (kind == "sheet") {
            SpriteSheetDefinition sheet_def;
            if (!(in >> sheet_def.id >> sheet_def.texture_id >> sheet_def.grid.tile_w >> sheet_def.grid.tile_h)) {
                return fail("malformed sheet");
            }
            in >> sheet_def.grid.spacing >> sheet_def.grid.margin;
            catalog.add_sheet(std::move(sheet_def));
            continue;
        }

        if (kind == "sprite") {
            SpriteDefinition sprite_def;
            if (!(in >> sprite_def.id >> sprite_def.texture_id
                     >> sprite_def.source.x >> sprite_def.source.y >> sprite_def.source.w >> sprite_def.source.h)) {
                return fail("malformed sprite");
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
            if (path) {
                warn_empty_direct_sprite(*path, line_no, sprite_def);
            }
            catalog.add(std::move(sprite_def));
            continue;
        }

        if (kind == "sheet_sprite") {
            SpriteDefinition sprite_def;
            if (!(in >> sprite_def.id >> sprite_def.sheet_id >> sprite_def.frame)) {
                return fail("malformed sheet_sprite");
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

        if (kind == "animation") {
            SpriteAnimationDefinition animation;
            std::string playback;
            if (!(in >> animation.id >> playback >> animation.fps) || !parse_playback(playback, animation.playback) ||
                !(animation.fps > 0.0f)) {
                return fail("malformed animation (animation <id> loop|once|pingpong <fps> <sprite>[*hold]...)");
            }
            for (std::string token; in >> token;) {
                SpriteAnimationFrame frame;
                if (!parse_animation_frame(token, frame)) {
                    return fail("malformed animation frame '" + token + "'");
                }
                animation.frames.push_back(std::move(frame));
            }
            catalog.add_animation(std::move(animation));
            continue;
        }

        return fail("unknown directive");
    }
    return true;
}

} // namespace

f32 sprite_animation_length(const SpriteAnimationDefinition& animation) {
    if (animation.frames.empty() || !(animation.fps > 0.0f)) {
        return 0.0f;
    }
    f32 holds = 0.0f;
    for (const SpriteAnimationFrame& frame : animation.frames) {
        holds += std::max(frame.hold, 0.0f);
    }
    // There and back: its first and last frames shown once a turn.
    if (animation.playback == SpritePlayback::PingPong && animation.frames.size() > 1) {
        holds += holds - std::max(animation.frames.front().hold, 0.0f) - std::max(animation.frames.back().hold, 0.0f);
    }
    return holds / animation.fps;
}

i32 sprite_animation_frame(const SpriteAnimationDefinition& animation, f32 time, bool* finished) {
    if (finished) {
        *finished = false;
    }
    const auto count = static_cast<i32>(animation.frames.size());
    const f32 length = sprite_animation_length(animation);
    if (count == 0) {
        return -1;
    }
    if (!(length > 0.0f)) {
        return 0;
    }
    f32 t = time;
    if (animation.playback == SpritePlayback::Once) {
        if (t >= length) {
            if (finished) {
                *finished = true;
            }
            return count - 1;
        }
        t = std::max(t, 0.0f);
    } else {
        t = std::fmod(t, length);
        if (t < 0.0f) {
            t += length;
        }
    }
    // Its frames there (and, for PingPong, back: the last but one to the second).
    const auto hold = [&](i32 index) { return std::max(animation.frames[static_cast<std::size_t>(index)].hold, 0.0f) / animation.fps; };
    for (i32 index = 0; index < count; ++index) {
        t -= hold(index);
        if (t < 0.0f) {
            return index;
        }
    }
    for (i32 index = count - 2; index > 0; --index) {
        t -= hold(index);
        if (t < 0.0f) {
            return index;
        }
    }
    return animation.playback == SpritePlayback::PingPong && count > 1 ? 1 : count - 1; // rounding, at the very end
}

bool SpriteRef::valid() const {
    return catalog && !id.empty() && catalog->contains(id);
}

void SpriteCatalog::clear() {
    _textures.clear();
    _sheets.clear();
    _sprites.clear();
    _animations.clear();
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

void SpriteCatalog::add_animation(SpriteAnimationDefinition animation) {
    _animations[animation.id] = std::move(animation);
}

bool SpriteCatalog::remove_texture(std::string_view id) {
    return _textures.erase(std::string{id}) > 0;
}

bool SpriteCatalog::remove_sheet(std::string_view id) {
    return _sheets.erase(std::string{id}) > 0;
}

bool SpriteCatalog::remove(std::string_view sprite_id) {
    return _sprites.erase(std::string{sprite_id}) > 0;
}

bool SpriteCatalog::remove_animation(std::string_view id) {
    return _animations.erase(std::string{id}) > 0;
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

const SpriteAnimationDefinition* SpriteCatalog::animation(std::string_view id) const {
    const auto found = _animations.find(std::string{id});
    return found == _animations.end() ? nullptr : &found->second;
}

std::vector<std::string> SpriteCatalog::animation_ids() const {
    std::vector<std::string> ids;
    ids.reserve(_animations.size());
    for (const auto& [id, animation] : _animations) {
        ids.push_back(id);
    }
    std::ranges::sort(ids);
    return ids;
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
    const std::optional<std::string> text = read_content_text(path);
    if (!text) {
        KIN_LOG_ERROR_F("asset",
                        "sprite catalog open failed",
                        (LogFields{
                            {.name = "path", .value = path.string()},
                            {.name = "type", .value = "SpriteCatalog"},
                        }));
        throw std::runtime_error("Failed to open sprite catalog: " + path.string());
    }
    SpriteCatalog catalog;
    if (std::string error; !read_catalog(*text, &path, catalog, error)) {
        // "line 3: ..." as "<path>:3: ..."
        throw std::runtime_error(path.string() + ":" + error.substr(std::string_view{"line "}.size()));
    }
    for (const auto& [id, animation] : catalog.animations()) {
        for (const SpriteAnimationFrame& frame : animation.frames) {
            if (!catalog.contains(frame.sprite)) {
                KIN_LOG_WARN_F("asset",
                               "sprite catalog animation shows a sprite it has not",
                               (LogFields{
                                   {.name = "path", .value = path.string()},
                                   {.name = "type", .value = "SpriteCatalog"},
                                   {.name = "animation", .value = id},
                                   {.name = "sprite", .value = frame.sprite},
                               }));
                break;
            }
        }
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

bool parse_sprite_catalog(std::string_view text, SpriteCatalog& out, std::string& error) {
    SpriteCatalog catalog;
    if (!read_catalog(text, nullptr, catalog, error)) {
        return false;
    }
    out = std::move(catalog);
    return true;
}

std::string serialize_sprite_catalog(const SpriteCatalog& catalog) {
    const auto sorted = [](const auto& by_id) {
        std::vector<const typename std::decay_t<decltype(by_id)>::mapped_type*> out;
        out.reserve(by_id.size());
        for (const auto& [id, value] : by_id) {
            out.push_back(&value);
        }
        std::ranges::sort(out, [](const auto* a, const auto* b) { return a->id < b->id; });
        return out;
    };
    const auto textures = sorted(catalog.textures());
    const auto sheets = sorted(catalog.sheets());
    const auto sprites = sorted(catalog.sprites());
    const auto animations = sorted(catalog.animations());

    std::ostringstream out;
    out << "# kin sprite catalog\n";
    for (const TextureAssetRef* texture : textures) {
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
    if (!animations.empty() && (!textures.empty() || !sheets.empty() || !sprites.empty())) {
        out << '\n';
    }
    for (const SpriteAnimationDefinition* animation : animations) {
        out << "animation " << animation->id << ' ' << playback_text(animation->playback) << ' ' << animation->fps;
        for (const SpriteAnimationFrame& frame : animation->frames) {
            out << ' ' << frame.sprite;
            if (frame.hold != 1.0f) {
                out << '*' << frame.hold;
            }
        }
        out << '\n';
    }
    return out.str();
}

bool save_sprite_catalog(const SpriteCatalog& catalog, const std::filesystem::path& path) {
    if (std::ranges::any_of(catalog.textures(), [](const auto& texture) { return texture.second.path.empty(); })) {
        return false; // a texture made in code: nothing to name it by
    }
    std::ofstream out(path);
    if (!out) {
        return false;
    }
    out << serialize_sprite_catalog(catalog);
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
