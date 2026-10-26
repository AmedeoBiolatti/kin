#include <kin/ecs/assets2d.hpp>

#include <kin/ecs/animator.hpp>

#include <kin/anim/anim_format.hpp>
#include <kin/anim/binding.hpp>
#include <kin/assets/content.hpp>
#include <kin/assets/image.hpp>
#include <kin/assets/texture_file.hpp>
#include <kin/ecs/particles.hpp>
#include <kin/ecs/render.hpp>
#include <kin/ecs/text.hpp>
#include <kin/ecs/tilemap.hpp>
#include <kin/particles/particle_catalog.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <kin/platform/log.hpp>
#include <kin/renderer/sprite_catalog.hpp>
#include <kin/renderer/svg.hpp>
#if KIN_ENABLE_AUDIO
#include <kin/audio/audio_catalog.hpp>
#include <kin/ecs/audio.hpp>
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

namespace kin {

namespace {

// Said once each time it fails anew (a file fixed and broken again is said again).
void tell(std::map<std::string, std::string>& problems, const std::string& path, std::string why) {
    const auto [found, fresh] = problems.insert_or_assign(path, std::move(why));
    if (fresh) {
        KIN_LOG_WARN_F("asset", "asset could not be read",
                       (LogFields{{.name = "path", .value = path}, {.name = "error", .value = found->second}}));
    }
}

std::string lowercase_extension(std::string_view path) {
    std::string ext = std::filesystem::path(path).extension().string();
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

} // namespace

Assets2D::Assets2D(Renderer2D& renderer, std::filesystem::path root)
    : _renderer(renderer),
      _manager(std::move(root)) {
}

std::optional<std::filesystem::file_time_type> Assets2D::file_time(std::string_view path) const {
    std::error_code error;
    const std::filesystem::file_time_type time = std::filesystem::last_write_time(_manager.resolve(path), error);
    return error ? std::nullopt : std::optional(time); // in a pack, or gone
}

Texture Assets2D::texture(std::string_view path) {
    if (path.empty()) {
        return {};
    }
    if (const auto found = _textures.find(path); found != _textures.end()) {
        return found->second.texture;
    }
    std::string key(path);
    Made made{.texture = {}, .time = file_time(path), .failed = false};
    const std::filesystem::path file = _manager.resolve(path);
    try {
        const std::string ext = lowercase_extension(path);
        if (ext == ".ktx2" || ext == ".dds") {
            // As the GPU keeps it (texture_file.hpp).
            const std::optional<std::string> bytes = read_content_file(file);
            if (!bytes) {
                throw std::runtime_error("it could not be read");
            }
            std::string error;
            const std::optional<CompressedImage> image =
                decode_compressed_image({reinterpret_cast<const u8*>(bytes->data()), bytes->size()}, &error);
            if (!image) {
                throw std::runtime_error(error);
            }
            made.texture = _renderer.create_texture_compressed(*image);
        } else {
            const Image image = load_image(file);
            if (!image.valid()) {
                throw std::runtime_error("it is not an image kin reads");
            }
            made.texture = _renderer.create_texture_from_rgba(image.rgba.data(), image.size);
        }
        if (!made.texture) {
            throw std::runtime_error("the renderer could not make a texture of it");
        }
        _problems.erase(key);
    } catch (const std::exception& error) {
        made.failed = true;
        tell(_problems, key, error.what());
    }
    ++_version;
    return _textures.emplace(std::move(key), std::move(made)).first->second.texture;
}

std::shared_ptr<const SpriteCatalog> Assets2D::sprite_catalog(std::string_view path) {
    if (path.empty()) {
        return nullptr;
    }
    if (const auto found = _sprite_catalogs.find(path); found != _sprite_catalogs.end()) {
        return found->second.catalog;
    }
    std::string key(path);
    Read<SpriteCatalog> read{.catalog = nullptr, .files = {{key, file_time(path)}}};
    try {
        // Its images are named from the content root, as the catalog's own path is.
        auto catalog = std::make_shared<SpriteCatalog>(load_sprite_catalog(_manager, _renderer, _manager.resolve(path)));
        for (const auto& [id, texture] : catalog->textures()) {
            if (!texture.path.empty()) {
                read.files.emplace_back(texture.path, file_time(texture.path));
                _manager.unload(texture.path); // made a texture: its pixels are not kept
            }
        }
        read.catalog = std::move(catalog);
        _problems.erase(key);
    } catch (const std::exception& error) {
        tell(_problems, key, error.what());
    }
    ++_version;
    return _sprite_catalogs.emplace(std::move(key), std::move(read)).first->second.catalog;
}

std::shared_ptr<const AudioCatalog> Assets2D::audio_catalog(std::string_view path) {
#if KIN_ENABLE_AUDIO
    if (path.empty()) {
        return nullptr;
    }
    if (const auto found = _audio_catalogs.find(path); found != _audio_catalogs.end()) {
        return found->second.catalog;
    }
    std::string key(path);
    Read<AudioCatalog> read{.catalog = nullptr, .files = {{key, file_time(path)}}};
    try {
        if (infer_asset_type(std::filesystem::path(path)) == AssetType::Audio) {
            // A sound file: a catalog of one cue, named as the file is.
            if (!content_file_exists(_manager.resolve(path))) {
                throw std::runtime_error("there is no such file");
            }
            auto catalog = std::make_shared<AudioCatalog>();
            catalog->set_root(_manager.root());
            catalog->add_clip({.id = key, .path = key});
            catalog->add_cue({.id = key, .clips = {key}});
            read.catalog = std::move(catalog);
        } else {
            read.catalog = std::make_shared<AudioCatalog>(load_audio_catalog(_manager.resolve(path)));
        }
        _problems.erase(key);
    } catch (const std::exception& error) {
        tell(_problems, key, error.what());
    }
    ++_version;
    return _audio_catalogs.emplace(std::move(key), std::move(read)).first->second.catalog;
#else
    static_cast<void>(path);
    return nullptr;
#endif
}

std::shared_ptr<const ParticleCatalog> Assets2D::particle_catalog(std::string_view path) {
    if (path.empty()) {
        return nullptr;
    }
    if (const auto found = _particle_catalogs.find(path); found != _particle_catalogs.end()) {
        return found->second.catalog;
    }
    std::string key(path);
    Read<ParticleCatalog> read{.catalog = nullptr, .files = {{key, file_time(path)}}};
    try {
        read.catalog = std::make_shared<ParticleCatalog>(load_particle_catalog(_manager.resolve(path)));
        _problems.erase(key);
    } catch (const std::exception& error) {
        tell(_problems, key, error.what());
    }
    ++_version;
    return _particle_catalogs.emplace(std::move(key), std::move(read)).first->second.catalog;
}

std::shared_ptr<const TilemapAsset> Assets2D::tilemap(std::string_view path) {
    if (path.empty()) {
        return nullptr;
    }
    if (const auto found = _tilemaps.find(path); found != _tilemaps.end()) {
        return found->second.catalog;
    }
    std::string key(path);
    Read<TilemapAsset> read{.catalog = nullptr, .files = {{key, file_time(path)}}};
    std::string error;
    TileMap map = load_tilemap(_manager.resolve(path), root(), &error);
    if (map.cols <= 0 || map.rows <= 0) {
        tell(_problems, key, error.empty() ? std::string("it is not a tilemap (no cells)") : error);
    } else {
        if (!error.empty()) {
            tell(_problems, key, error); // its tileset: drawn without its tiles until it reads
        } else {
            _problems.erase(key);
        }
        // Read again when its tileset, or the image of it, changes too.
        if (!map.tileset_source.empty()) {
            read.files.emplace_back(map.tileset_source, file_time(map.tileset_source));
        }
        auto made = std::make_shared<TilemapAsset>();
        if (const std::string image = map.tileset.image; !image.empty()) {
            read.files.emplace_back(image, file_time(image));
            made->sheet = SpriteSheet(texture(image), map.tileset.sheet);
        }
        made->map = std::move(map);
        read.catalog = std::move(made);
    }
    ++_version;
    return _tilemaps.emplace(std::move(key), std::move(read)).first->second.catalog;
}

std::shared_ptr<const Shape> Assets2D::svg(std::string_view path) {
    if (path.empty()) {
        return nullptr;
    }
    if (const auto found = _svgs.find(path); found != _svgs.end()) {
        return found->second.catalog;
    }
    std::string key(path);
    Read<Shape> read{.catalog = nullptr, .files = {{key, file_time(path)}}};
    std::string error;
    if (std::optional<Shape> shape = load_svg(_manager.resolve(path), &error)) {
        read.catalog = std::make_shared<const Shape>(std::move(*shape));
        _problems.erase(key);
    } else {
        tell(_problems, key, error.empty() ? std::string("it is not an SVG file") : error);
    }
    ++_version;
    return _svgs.emplace(std::move(key), std::move(read)).first->second.catalog;
}

ui2::Font Assets2D::font(std::string_view path, f32 size) {
    size = std::clamp(size, 1.0f, 512.0f);
    std::string key = std::string(path) + "@" + std::to_string(size);
    if (const auto found = _fonts.find(key); found != _fonts.end()) {
        return found->second.catalog ? *found->second.catalog : ui2::Font{};
    }
    Read<ui2::Font> read;
    if (path.empty()) {
        read.catalog = std::make_shared<const ui2::Font>(ui2::system_ui_font(size, ui2::TextRendering::Sdf));
    } else {
        read.files = {{std::string(path), file_time(path)}};
        try {
            read.catalog = std::make_shared<const ui2::Font>(
                ui2::load_ttf_font(_manager.resolve(path), size, ui2::TtfFontOptions{.rendering = ui2::TextRendering::Sdf}));
            _problems.erase(std::string(path));
        } catch (const std::exception& error) {
            tell(_problems, std::string(path), error.what());
        }
    }
    ++_version;
    const auto& made = _fonts.emplace(std::move(key), std::move(read)).first->second.catalog;
    return made ? *made : ui2::Font{};
}

std::shared_ptr<AnimationAssetLibrary> Assets2D::animations(std::string_view path) {
    if (path.empty()) {
        return nullptr;
    }
    // Made here, not const: players resolve (and cache) their animations in it.
    const auto mutable_of = [](const std::shared_ptr<const AnimationAssetLibrary>& library) {
        return std::const_pointer_cast<AnimationAssetLibrary>(library);
    };
    if (const auto found = _animations.find(path); found != _animations.end()) {
        return mutable_of(found->second.catalog);
    }
    std::string key(path);
    Read<AnimationAssetLibrary> read{.catalog = nullptr, .files = {{key, file_time(path)}}};
    try {
        auto library = std::make_shared<AnimationAssetLibrary>();
        library->merge(load_animation_registry_fragment(_manager.resolve(path)));
        read.catalog = std::move(library);
        _problems.erase(key);
    } catch (const std::exception& error) {
        tell(_problems, key, error.what());
    }
    ++_version;
    return mutable_of(_animations.emplace(std::move(key), std::move(read)).first->second.catalog);
}

const PropertyRegistry& Assets2D::properties(EcsWorld& world) {
    Properties& made = _properties[&world];
    const std::size_t described = world.components().descriptors().size();
    if (!made.registry || made.described != described) {
        made.registry = std::make_shared<PropertyRegistry>();
        register_builtin_properties(*made.registry, world);
        register_described_properties(*made.registry, world);
        made.described = described;
    }
    return *made.registry;
}

void Assets2D::add_binder(std::string name, Binder binder) {
    const auto found = std::ranges::find(_binders, name, &std::pair<std::string, Binder>::first);
    if (found != _binders.end()) {
        found->second = std::move(binder);
    } else {
        _binders.emplace_back(std::move(name), std::move(binder));
    }
}

bool Assets2D::bind(EcsWorld& world) {
    _bound_changed = false;
    bind_texture_renderers(world, *this);
    bind_sprite_renderers(world, *this);
    bind_audio_emitters(world, *this);
    bind_particle_systems(world, *this);
    bind_particle_fields(world, *this);
    bind_tilemaps(world, *this);
    bind_shape_renderers(world, *this);
    bind_text_renderers(world, *this);
    bind_animators(world, *this);
    for (auto& [name, binder] : _binders) {
        if (binder) {
            binder(world, *this);
        }
    }
    return std::exchange(_bound_changed, false);
}

std::vector<std::string> Assets2D::reload_changed() {
    std::vector<std::string> changed;
    for (auto made = _textures.begin(); made != _textures.end();) {
        if (file_time(made->first) != made->second.time) {
            changed.push_back(made->first);
            made = _textures.erase(made);
        } else {
            ++made;
        }
    }
    // Catalogs: when they, or the files they were made of, changed.
    const auto reread = [&](auto& catalogs) {
        for (auto read = catalogs.begin(); read != catalogs.end();) {
            const bool stale = std::ranges::any_of(read->second.files, [&](const auto& file) { return file_time(file.first) != file.second; });
            if (stale) {
                changed.push_back(read->first);
                if (read->second.catalog) {
                    _retired.push_back(read->second.catalog);
                }
                read = catalogs.erase(read);
            } else {
                ++read;
            }
        }
    };
    reread(_sprite_catalogs);
    reread(_audio_catalogs);
    reread(_particle_catalogs);
    reread(_tilemaps);
    reread(_svgs);
    reread(_fonts);
    reread(_animations);
    // Those read through manager(): as they were when first seen here.
    for (const std::string& path : _manager.loaded_asset_paths()) {
        const std::optional<std::filesystem::file_time_type> now = file_time(path);
        const auto [seen, first] = _read.try_emplace(path, now);
        if (!first && seen->second != now) {
            _manager.unload(path);
            _read.erase(seen);
            changed.push_back(path);
        }
    }
    if (!changed.empty()) {
        ++_version;
    }
    return changed;
}

void bind_sprite_renderers(EcsWorld& world, Assets2D& assets) {
    std::vector<std::pair<flecs::entity, const SpriteCatalog*>> rebound;
    world.raw().each([&](flecs::entity entity, const SpriteRenderer& renderer) {
        if (renderer.asset.id.empty()) {
            return;
        }
        const SpriteCatalog* catalog = assets.sprite_catalog(renderer.asset.id).get();
        if (catalog != renderer.sprite.catalog) {
            rebound.emplace_back(entity, catalog);
        }
    });
    for (const auto& [entity, catalog] : rebound) {
        EcsEntity owner{entity}; // one from a prefab gets its own copy first
        owner.get_mut<SpriteRenderer>()->sprite.catalog = catalog; // its id kept
        owner.modified<SpriteRenderer>();
    }
    if (!rebound.empty()) {
        assets.changed();
    }
}

void bind_audio_emitters([[maybe_unused]] EcsWorld& world, [[maybe_unused]] Assets2D& assets) {
#if KIN_ENABLE_AUDIO
    std::vector<std::pair<flecs::entity, const AudioCatalog*>> rebound;
    world.raw().each([&](flecs::entity entity, const AudioEmitter& emitter) {
        if (emitter.asset.id.empty()) {
            return;
        }
        const AudioCatalog* catalog = assets.audio_catalog(emitter.asset.id).get();
        if (catalog != emitter.catalog) {
            rebound.emplace_back(entity, catalog);
        }
    });
    for (const auto& [entity, catalog] : rebound) {
        EcsEntity owner{entity}; // one from a prefab gets its own copy first
        owner.get_mut<AudioEmitter>()->catalog = catalog;
        owner.modified<AudioEmitter>();
    }
    if (!rebound.empty()) {
        assets.changed();
    }
#endif
}

void bind_texture_renderers(EcsWorld& world, Assets2D& assets) {
    // Read only, so nothing is marked written; those to change, changed after.
    std::vector<std::pair<flecs::entity, Texture>> rebound;
    world.raw().each([&](flecs::entity entity, const TextureRenderer& renderer) {
        if (renderer.asset.id.empty()) {
            return; // set from C++, if at all
        }
        Texture texture = assets.texture(renderer.asset.id);
        if (!(texture == renderer.texture)) {
            rebound.emplace_back(entity, std::move(texture));
        }
    });
    for (auto& [entity, texture] : rebound) {
        EcsEntity owner{entity}; // one from a prefab gets its own copy first
        owner.get_mut<TextureRenderer>()->texture = std::move(texture);
        owner.modified<TextureRenderer>();
    }
    if (!rebound.empty()) {
        assets.changed();
    }
}

void bind_particle_systems(EcsWorld& world, Assets2D& assets) {
    struct Rebound {
        flecs::entity entity;
        std::optional<std::shared_ptr<const ParticleCatalog>> catalog; // when it changed
        const SpriteCatalog* sprites = nullptr;
    };
    std::vector<Rebound> rebound;
    world.raw().each([&](flecs::entity entity, const ParticleSystemComponent& particles) {
        Rebound item{.entity = entity, .catalog = std::nullopt, .sprites = particles.sprites};
        if (!particles.asset.id.empty()) {
            std::shared_ptr<const ParticleCatalog> catalog = assets.particle_catalog(particles.asset.id);
            if (catalog.get() != particles.system.catalog()) {
                item.catalog = std::move(catalog);
            }
        }
        if (!particles.sprites_asset.id.empty()) {
            item.sprites = assets.sprite_catalog(particles.sprites_asset.id).get();
        }
        if (item.catalog || item.sprites != particles.sprites) {
            rebound.push_back(std::move(item));
        }
    });
    for (Rebound& item : rebound) {
        EcsEntity owner{item.entity}; // one from a prefab gets its own copy first
        ParticleSystemComponent* particles = owner.get_mut<ParticleSystemComponent>();
        if (item.catalog) {
            particles->system.set_catalog(std::move(*item.catalog)); // its particles kept
        }
        particles->sprites = item.sprites;
        owner.modified<ParticleSystemComponent>();
    }
    if (!rebound.empty()) {
        assets.changed();
    }
}

void bind_particle_fields(EcsWorld& world, Assets2D& assets) {
    struct Rebound {
        flecs::entity entity;
        std::shared_ptr<const ParticleCatalog> catalog; // keeps `field` while it is made
        const ParticleField* field = nullptr;
        const SpriteCatalog* sprites = nullptr;
    };
    std::vector<Rebound> rebound;
    world.raw().each([&](flecs::entity entity, const ParticleFieldComponent& component) {
        Rebound item{.entity = entity, .catalog = nullptr, .field = component.bound, .sprites = component.sprites};
        if (!component.asset.id.empty()) {
            item.catalog = assets.particle_catalog(component.asset.id);
            item.field = item.catalog ? item.catalog->field(component.name) : nullptr;
        }
        if (!component.sprites_asset.id.empty()) {
            item.sprites = assets.sprite_catalog(component.sprites_asset.id).get();
        }
        if (item.field != component.bound || item.sprites != component.sprites) {
            rebound.push_back(std::move(item));
        }
    });
    for (const Rebound& item : rebound) {
        EcsEntity owner{item.entity};
        ParticleFieldComponent* component = owner.get_mut<ParticleFieldComponent>();
        if (item.field != component->bound) {
            // Its area from where the entity is.
            ParticleField placed = item.field ? *item.field : ParticleField{};
            if (item.field && item.entity.has<Transform2D>()) {
                const Vec2f at = current_world_transform(item.entity).pos;
                placed.area.x += at.x;
                placed.area.y += at.y;
            }
            reset_particle_field(component->state, placed);
            component->bound = item.field;
        }
        component->sprites = item.sprites;
        owner.modified<ParticleFieldComponent>();
    }
    if (!rebound.empty()) {
        assets.changed();
    }
}

void bind_tilemaps(EcsWorld& world, Assets2D& assets) {
    std::vector<std::pair<flecs::entity, std::shared_ptr<const TilemapAsset>>> rebound;
    world.raw().each([&](flecs::entity entity, const TilemapComponent& tilemap) {
        if (tilemap.asset.id.empty()) {
            return; // set from C++, if at all
        }
        std::shared_ptr<const TilemapAsset> read = assets.tilemap(tilemap.asset.id);
        if (read != tilemap.tilemap) {
            rebound.emplace_back(entity, std::move(read));
        }
    });
    for (auto& [entity, read] : rebound) {
        EcsEntity owner{entity}; // one from a prefab gets its own copy first
        owner.get_mut<TilemapComponent>()->tilemap = std::move(read);
        owner.modified<TilemapComponent>();
    }
    if (!rebound.empty()) {
        assets.changed();
    }
}

void bind_shape_renderers(EcsWorld& world, Assets2D& assets) {
    struct Remade {
        flecs::entity entity;
        std::shared_ptr<const Shape> svg;
    };
    std::vector<Remade> remade;
    world.raw().each([&](flecs::entity entity, const ShapeRenderer& shape) {
        if (shape.spec.kind == ShapeKind::None) {
            return; // its mesh is the game's own
        }
        std::shared_ptr<const Shape> svg = shape.spec.kind == ShapeKind::Svg ? assets.svg(shape.asset.id) : nullptr;
        if (shape.mesh && shape.made == shape.spec && shape.made_from == svg.get()) {
            return;
        }
        remade.push_back({entity, std::move(svg)});
    });
    for (const Remade& made : remade) {
        EcsEntity owner{made.entity}; // one from a prefab gets its own copy first
        ShapeRenderer& shape = *owner.get_mut<ShapeRenderer>();
        shape.mesh = std::make_shared<const ShapeMesh>(make_shape(shape.spec, made.svg.get()).mesh());
        shape.made = shape.spec;
        shape.made_from = made.svg.get();
        owner.modified<ShapeRenderer>();
    }
    if (!remade.empty()) {
        assets.changed();
    }
}

void bind_text_renderers(EcsWorld& world, Assets2D& assets) {
    std::vector<std::pair<flecs::entity, ui2::Font>> rebound;
    world.raw().each([&](flecs::entity entity, const TextRenderer& text) {
        ui2::Font face = assets.font(text.font.id, text.size);
        if (face.identity() != text.face.identity()) {
            rebound.emplace_back(entity, std::move(face));
        }
    });
    for (auto& [entity, face] : rebound) {
        EcsEntity owner{entity}; // one from a prefab gets its own copy first
        owner.get_mut<TextRenderer>()->face = std::move(face);
        owner.modified<TextRenderer>();
    }
    if (!rebound.empty()) {
        assets.changed();
    }
}

void bind_animators(EcsWorld& world, Assets2D& assets) {
    struct Rebound {
        flecs::entity entity;
        std::shared_ptr<AnimationAssetLibrary> library;
    };
    std::vector<flecs::entity> animated;
    world.raw().each([&](flecs::entity entity, const Animator&) { animated.push_back(entity); });
    if (animated.empty()) {
        return;
    }
    // Made outside the walk: making it may register components.
    const PropertyRegistry* properties = &assets.properties(world);
    std::vector<Rebound> rebound;
    for (const flecs::entity entity : animated) {
        const Animator& animator = *entity.get<Animator>();
        std::shared_ptr<AnimationAssetLibrary> library = assets.animations(animator.asset.id);
        if (library != animator.library || animator.properties != properties) {
            rebound.push_back({entity, std::move(library)});
        }
    }
    for (Rebound& made : rebound) {
        EcsEntity owner{made.entity}; // one from a prefab gets its own copy first
        Animator& animator = *owner.get_mut<Animator>();
        animator.library = std::move(made.library);
        animator.properties = properties;
    }
    if (!rebound.empty()) {
        assets.changed();
    }
}

} // namespace kin
