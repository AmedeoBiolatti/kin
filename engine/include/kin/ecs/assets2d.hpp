#pragma once

// The assets a 2D world's components name by path (ComponentAssetRef fields,
// as scene files write them), read from a content root, shared, and read
// again when their files change. Components get what they name through
// binders: kin's are built in (TextureRenderer.asset: an image, as a texture;
// SpriteRenderer.asset: a sprite catalog; AudioEmitter.asset: a sound or an
// audio catalog; ParticleSystemComponent.asset and ParticleFieldComponent.asset:
// a particle catalog; TilemapComponent.asset: a tilemap, with its tileset and
// that's image; ShapeRenderer: its mesh, made of its shape or SVG file;
// TextRenderer.font: a font), and a game adds one for each component of its
// own that names an asset.
//
//     kin::Assets2D assets{renderer, kin::content_root()};
//     assets.add_binder("Speaker", [](kin::EcsWorld& world, kin::Assets2D& assets) {
//         world.each([&](Speaker& speaker) { speaker.sound = assets.manager().load<Sound>(speaker.asset.id); });
//     });
//     assets.reload_changed(); // now and then: files saved since are read again
//     assets.bind(world);      // before drawing
//
// EcsScene does this for its world (EcsSceneRenderConfig::assets), so a scene
// file naming an image shows it. Renderer thread.

#include <kin/assets/asset_manager.hpp>
#include <kin/ecs/world.hpp>
#include <kin/renderer/texture.hpp>
#include <kin/ui2/text.hpp>

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kin {

class AudioCatalog;
class ParticleCatalog;
class Renderer2D;
class Shape;
class SpriteCatalog;
struct TilemapAsset;

class Assets2D {
public:
    // Gives the components of one kind what they name, for a whole world.
    using Binder = std::function<void(EcsWorld&, Assets2D&)>;

    Assets2D(Renderer2D& renderer, std::filesystem::path root);
    Assets2D(const Assets2D&) = delete;
    Assets2D& operator=(const Assets2D&) = delete;

    const std::filesystem::path& root() const { return _manager.root(); }
    // What reads assets of any kind (AssetManager's loaders: images, sprite
    // catalogs, ...; a game registers its own), for binders.
    AssetManager& manager() { return _manager; }
    Renderer2D& renderer() { return _renderer; }

    // An image as a texture (PNG, JPEG, BMP, ...; KTX2 and DDS as compressed
    // textures), made once and shared. Invalid when it cannot be read: why is
    // in problems(), and it is tried again once its file changes.
    Texture texture(std::string_view path);
    // A sprite catalog (.kinsprites) with its images made textures; read again
    // when it or one of its images changes. Null when it cannot be read.
    std::shared_ptr<const SpriteCatalog> sprite_catalog(std::string_view path);
    // An audio catalog (.kinaudio), or a sound file (WAV, Ogg, ...) as a
    // catalog of one cue named by its path. Null when it cannot be read (or
    // kin was built without audio).
    std::shared_ptr<const AudioCatalog> audio_catalog(std::string_view path);
    // A particle catalog (.kinparticles): effects and fields. Null when it
    // cannot be read.
    std::shared_ptr<const ParticleCatalog> particle_catalog(std::string_view path);
    // A tilemap (.kintilemap), with the tileset file it names (.kintileset)
    // and the image its tileset's tiles are cut from (a texture, as a sheet);
    // read again when any of them changes. Null when the map cannot be read
    // (a map whose tileset can't be read is kept: its problem is told).
    std::shared_ptr<const TilemapAsset> tilemap(std::string_view path);
    // An SVG-lite file (.svg) as a shape. Null when it cannot be read.
    std::shared_ptr<const Shape> svg(std::string_view path);
    // A font file (.ttf, .otf) at `size` points, drawn from distance fields
    // (sharp as it is scaled up) where the renderer can; `path` empty: the
    // system's UI font. Invalid when it cannot be read.
    ui2::Font font(std::string_view path, f32 size);

    // A binder for a game's own component (by name; one of that name replaces it).
    void add_binder(std::string name, Binder binder);
    // Each component naming an asset gets it: TextureRenderer's, then the
    // game's binders'. True when a component got a different one (a draw cache
    // of the world is stale).
    bool bind(EcsWorld& world);

    // Assets read before whose files changed since: forgotten, so the next
    // texture() or load reads them again (bind() then gives the new ones out).
    // The paths changed. Checks each file's time: call it now and then, not
    // every frame of a large game.
    std::vector<std::string> reload_changed();

    // Why an asset could not be read, by its path.
    const std::map<std::string, std::string>& problems() const { return _problems; }
    // Bumped when a texture is made or read again.
    u64 version() const { return _version; }

    // Bumped by bind() when a binder changed a component: a binder sets it.
    void changed() { _bound_changed = true; }

private:
    using FileTime = std::optional<std::filesystem::file_time_type>;
    struct Made {
        Texture texture;
        FileTime time; // the file's when read
        bool failed = false;
    };
    // A catalog, and the files it was made of (itself first) with their times.
    template <typename Catalog>
    struct Read {
        std::shared_ptr<const Catalog> catalog;
        std::vector<std::pair<std::string, FileTime>> files;
    };

    // Looked up by a path's view: binding every frame makes no strings.
    struct PathHash {
        using is_transparent = void;
        std::size_t operator()(std::string_view path) const { return std::hash<std::string_view>{}(path); }
    };

    std::optional<std::filesystem::file_time_type> file_time(std::string_view path) const;

    Renderer2D& _renderer;
    AssetManager _manager;
    std::unordered_map<std::string, Made, PathHash, std::equal_to<>> _textures;
    std::unordered_map<std::string, Read<SpriteCatalog>, PathHash, std::equal_to<>> _sprite_catalogs;
    std::unordered_map<std::string, Read<AudioCatalog>, PathHash, std::equal_to<>> _audio_catalogs;
    std::unordered_map<std::string, Read<ParticleCatalog>, PathHash, std::equal_to<>> _particle_catalogs;
    std::unordered_map<std::string, Read<TilemapAsset>, PathHash, std::equal_to<>> _tilemaps;
    std::unordered_map<std::string, Read<Shape>, PathHash, std::equal_to<>> _svgs;
    // By path and size ("path@size").
    std::unordered_map<std::string, Read<ui2::Font>, PathHash, std::equal_to<>> _fonts;
    // Catalogs read again: kept, as components and voices may still point at them.
    std::vector<std::shared_ptr<const void>> _retired;
    // Other assets read through manager(), with their files' times then.
    std::unordered_map<std::string, std::optional<std::filesystem::file_time_type>> _read;
    std::vector<std::pair<std::string, Binder>> _binders;
    std::map<std::string, std::string> _problems;
    u64 _version = 0;
    bool _bound_changed = false;
};

// kin's binders: a TextureRenderer's `asset`'s texture, a SpriteRenderer's
// catalog, an AudioEmitter's catalog, a particle system's catalog (and a
// field's state made from its catalog's field), a TilemapComponent's map,
// when they name one; a ShapeRenderer's mesh made again when its spec (or
// its SVG file) changed; a TextRenderer's font.
void bind_texture_renderers(EcsWorld& world, Assets2D& assets);
void bind_sprite_renderers(EcsWorld& world, Assets2D& assets);
void bind_audio_emitters(EcsWorld& world, Assets2D& assets);
void bind_particle_systems(EcsWorld& world, Assets2D& assets);
void bind_particle_fields(EcsWorld& world, Assets2D& assets);
void bind_tilemaps(EcsWorld& world, Assets2D& assets);
void bind_shape_renderers(EcsWorld& world, Assets2D& assets);
void bind_text_renderers(EcsWorld& world, Assets2D& assets);

} // namespace kin
