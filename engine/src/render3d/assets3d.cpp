#include <kin/render3d/assets3d.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <kin/render3d/material_asset3d.hpp>
#include <kin/assets/image.hpp>
#include <kin/renderer/renderer2d.hpp>

#include <algorithm>
#include <list>

namespace kin {

struct Assets3D::Data {
    struct ModelEntry {
        std::filesystem::path path;
        MeshOptions options;
        std::weak_ptr<AssetStorage<Model>> storage;
    };
    struct TextureEntry {
        std::filesystem::path path;
        u32 embedded_image;
        bool nearest, srgb;
        std::weak_ptr<ITextureBackend> texture;
    };
    struct MaterialEntry {
        std::filesystem::path path;
        std::string variant;
        std::weak_ptr<AssetStorage<MaterialBinding3D>> storage;
    };
    Renderer3D* renderer;
    std::filesystem::path root;
    Limits limits;
    Stats counters;
    std::list<ModelEntry> models;
    std::list<TextureEntry> textures;
    std::list<MaterialEntry> materials;
};

Assets3D::Assets3D(Renderer3D& renderer, std::filesystem::path root) : Assets3D(renderer, std::move(root), Limits{}) {}
Assets3D::Assets3D(Renderer3D& renderer, std::filesystem::path root, Limits limits) : _data(std::make_unique<Data>()) {
    _data->renderer = &renderer;
    _data->root = std::filesystem::absolute(root.empty() ? "." : root).lexically_normal();
    _data->limits = limits;
}
Assets3D::~Assets3D() = default;

const std::filesystem::path& Assets3D::root() const { return _data->root; }

void Assets3D::trim() {
    _data->models.remove_if([](const auto& entry) { return entry.storage.expired(); });
    _data->textures.remove_if([](const auto& entry) { return entry.texture.expired(); });
    _data->materials.remove_if([](const auto& entry) { return entry.storage.expired(); });
}
void Assets3D::clear() { _data->models.clear(); _data->textures.clear(); _data->materials.clear(); }
void Assets3D::invalidate(const std::filesystem::path& source) {
    const auto path = (_data->root / source).lexically_normal();
    std::vector<std::filesystem::path> owners{path};
    _data->models.remove_if([&](const auto& entry) {
        const auto storage = entry.storage.lock();
        const bool affected = entry.path == path || (storage &&
            std::ranges::find(storage->data.source_files, path) != storage->data.source_files.end());
        if (affected) owners.push_back(entry.path);
        return affected || !storage;
    });
    _data->textures.remove_if([&](const auto& entry) {
        return entry.texture.expired() || std::ranges::find(owners, entry.path) != owners.end();
    });
    _data->materials.remove_if([&](const auto& entry) {
        const auto storage = entry.storage.lock();
        return !storage || entry.path == path || std::ranges::find(storage->data.source_files, path) != storage->data.source_files.end();
    });
}
Assets3D::Stats Assets3D::stats() const {
    Stats s = _data->counters;
    s.model_entries = _data->models.size(); s.texture_entries = _data->textures.size();
    s.material_entries = _data->materials.size();
    return s;
}

AssetHandle<Model> Assets3D::load_model(const std::filesystem::path& path, std::string* error) {
    return load_model(path, MeshOptions{}, error);
}
AssetHandle<Model> Assets3D::load_model(const std::filesystem::path& path, const MeshOptions& options, std::string* error) {
    auto& d = *_data;
    const auto resolved = (d.root / path).lexically_normal();
    trim();
    for (auto it = d.models.begin(); it != d.models.end(); ++it) {
        if (it->path == resolved && it->options == options) {
            if (auto storage = it->storage.lock()) {
                ++d.counters.model_hits;
                d.models.splice(d.models.end(), d.models, it);
                if (error) error->clear();
                return AssetHandle<Model>{std::move(storage)};
            }
        }
    }
    ++d.counters.model_loads;
    auto storage = std::make_shared<AssetStorage<Model>>();
    storage->data = d.renderer->load_model_shared(resolved, options, error, this);
    if (!storage->data) return {};
    storage->is_loaded = true;
    if (d.limits.models) {
        while (d.models.size() >= d.limits.models) d.models.pop_front();
        d.models.push_back({resolved, options, storage});
    }
    return AssetHandle<Model>{std::move(storage)};
}

Texture Assets3D::load_texture(const std::filesystem::path& path, bool nearest, bool srgb, std::string* error) {
    const auto resolved = (_data->root / path).lexically_normal();
    auto result = share_texture(resolved, ~0u, nearest, srgb, [&] {
        auto image = load_image(resolved);
        Texture texture;
        if (image.valid()) {
            texture = srgb ? _data->renderer->renderer().create_texture_from_rgba(image.rgba.data(), image.size)
                          : _data->renderer->renderer().create_texture(image.size, TextureFormat::Rgba8, image.rgba.data());
            if (texture) _data->renderer->renderer().set_scale_mode(texture, nearest ? ScaleMode::Nearest : ScaleMode::Mipmapped);
        }
        return texture;
    });
    if (error) *error = result ? "" : "cannot decode/upload " + resolved.string();
    return result;
}

AssetHandle<MaterialBinding3D> Assets3D::load_materials(const std::filesystem::path& path, const std::shared_ptr<const Model>& model,
                                                     std::string_view variant, std::string* error) {
    auto& d = *_data;
    const auto resolved = (d.root / path).lexically_normal();
    trim();
    for (auto it = d.materials.begin(); it != d.materials.end(); ++it) {
        if (it->path == resolved && it->variant == variant) if (auto storage = it->storage.lock()) {
            if (storage->data.model.lock() == model && model) {
                ++d.counters.material_hits; d.materials.splice(d.materials.end(), d.materials, it);
                if (error) error->clear();
                return AssetHandle<MaterialBinding3D>{std::move(storage)};
            }
        }
    }
    ++d.counters.material_loads;
    try {
        auto loaded = load_materials3d_asset(resolved);
        if (!loaded.ok()) { if (error) *error = loaded.diagnostics.front(); return {}; }
        auto storage = std::make_shared<AssetStorage<MaterialBinding3D>>();
        storage->data = build_materials3d_binding(*loaded.asset, model, variant, *this);
        storage->is_loaded = true;
        if (d.limits.materials) {
            while (d.materials.size() >= d.limits.materials) d.materials.pop_front();
            d.materials.push_back({resolved, std::string(variant), storage});
        }
        if (error) error->clear();
        return AssetHandle<MaterialBinding3D>{std::move(storage)};
    } catch (const std::exception& failure) { if (error) *error = resolved.string() + ": " + failure.what(); }
    return {};
}

Texture Assets3D::share_texture(const std::filesystem::path& source, u32 embedded_image, bool nearest, bool srgb,
                              const std::function<Texture()>& load) {
    auto& d = *_data;
    const auto path = source.lexically_normal();
    for (auto it = d.textures.begin(); it != d.textures.end();) {
        auto backend = it->texture.lock();
        if (!backend) { it = d.textures.erase(it); continue; }
        if (it->path == path && it->embedded_image == embedded_image && it->nearest == nearest && it->srgb == srgb) {
            ++d.counters.texture_hits;
            d.textures.splice(d.textures.end(), d.textures, it);
            return Texture{std::move(backend)};
        }
        ++it;
    }
    ++d.counters.texture_loads;
    Texture texture = load();
    if (texture && d.limits.textures) {
        while (d.textures.size() >= d.limits.textures) d.textures.pop_front();
        d.textures.push_back({path, embedded_image, nearest, srgb, texture.backend()});
    }
    return texture;
}

} // namespace kin
