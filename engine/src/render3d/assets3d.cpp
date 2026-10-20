#include "material_asset3d_impl.hpp"

#include <algorithm>
#include <kin/assets/content.hpp>
#include <kin/assets/image.hpp>
#include <kin/assets/texture_file.hpp>
#include <kin/platform/log.hpp>
#include <kin/render3d/assets3d.hpp>
#include <kin/render3d/material_asset3d.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <limits>
#include <list>

namespace kin {

namespace {

// Without 3D (a headless session, a server) a model keeps what the CPU uses: its parts' names, places, skins and nodes,
// each with an empty mesh handle that still has its counts, bounds and joints, and its skeletons (and which is the node
// skeleton), clips and bounds. Nothing goes to the GPU and no material or texture is read, so a part's material is the
// default. Parts are refused as Renderer3D::create_mesh refuses them, so that part indices are a GPU load's.
Model3D cpu_model(const std::filesystem::path& path, std::string& why) {
    const auto bytes = read_content_file(path);
    if (!bytes) {
        why = "cannot read " + path.string();
        return {};
    }
    ModelData3D data;
    if (!read_model_data(*bytes, path, data, &why))
        return {};
    if (!data.asset_diagnostics.empty()) {
        why = diagnostics3d_text(data.asset_diagnostics);
        return {};
    }
    Model3D model;
    model.source_files.push_back(path.lexically_normal());
    bool first = true;
    for (ModelPartData3D& part : data.parts) {
        const std::size_t vertices = part.mesh.vertices.size(),
                          indices = part.mesh.indices.empty() ? vertices : part.mesh.indices.size();
        if (!part.mesh.validate() || vertices > std::numeric_limits<u32>::max() / sizeof(MeshVertex3D) ||
            indices > std::numeric_limits<u32>::max() / 4) {
            model.asset_diagnostics.push_back("model mesh could not be uploaded: " + part.name);
            continue;
        }
        const Bounds3D b = part.mesh.bounds();
        for (int i = 0; i < 8; ++i) {
            const Vec3f p = part.transform.apply(
                {(i & 1) ? b.max.x : b.min.x, (i & 2) ? b.max.y : b.min.y, (i & 4) ? b.max.z : b.min.z});
            model.bounds.min = first ? p
                                     : Vec3f{std::min(model.bounds.min.x, p.x), std::min(model.bounds.min.y, p.y),
                                             std::min(model.bounds.min.z, p.z)};
            model.bounds.max = first ? p
                                     : Vec3f{std::max(model.bounds.max.x, p.x), std::max(model.bounds.max.y, p.y),
                                             std::max(model.bounds.max.z, p.z)};
            first = false;
        }
        Mesh3D mesh{nullptr, static_cast<u32>(vertices),  static_cast<u32>(indices), b, {},
                    {},      part.mesh.skin_joint_count()};
        model.parts.push_back({std::move(part.name), std::move(mesh), Material3D{}, part.transform, part.skin,
                               part.node, part.node_offset});
    }
    model.skeletons = std::move(data.skeletons);
    model.clips = std::move(data.clips);
    model.node_skeleton = data.node_skeleton;
    if (model.parts.empty()) {
        why = path.string() + ": no triangles";
        return {};
    }
    if (!model.asset_diagnostics.empty()) {
        why = diagnostics3d_text(model.asset_diagnostics);
        return {};
    }
    return model;
}

// What a material names as its shader where nothing draws (a session without 3D): a handle, so that the material binds
// as it would with 3D, that no renderer is ever given.
struct CpuMaterialShader final : IMaterialShaderBackend3D {};

} // namespace

struct Assets3D::Data {
    Diagnostics3D errors;
    struct ModelEntry {
        std::filesystem::path path;
        MeshOptions3D options;
        std::weak_ptr<AssetStorage<Model3D>> storage;
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
    struct ShaderEntry {
        std::filesystem::path path;
        std::weak_ptr<IMaterialShaderBackend3D> shader;
    };
    std::list<ShaderEntry> shaders;
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

const std::filesystem::path& Assets3D::root() const {
    return _data->root;
}

void Assets3D::trim() {
    _data->shaders.remove_if([](const auto& entry) { return entry.shader.expired(); });
    _data->models.remove_if([](const auto& entry) { return entry.storage.expired(); });
    _data->textures.remove_if([](const auto& entry) { return entry.texture.expired(); });
    _data->materials.remove_if([](const auto& entry) { return entry.storage.expired(); });
}
void Assets3D::clear() {
    _data->shaders.clear();
    _data->models.clear();
    _data->textures.clear();
    _data->materials.clear();
}
void Assets3D::invalidate(const std::filesystem::path& source) {
    const auto path = (_data->root / source).lexically_normal();
    _data->shaders.remove_if([&](const auto& entry) { return entry.path == path || entry.shader.expired(); });
    std::vector<std::filesystem::path> owners{path};
    _data->models.remove_if([&](const auto& entry) {
        const auto storage = entry.storage.lock();
        const bool affected = entry.path == path || (storage && std::ranges::find(storage->data.source_files, path) !=
                                                                    storage->data.source_files.end());
        if (affected)
            owners.push_back(entry.path);
        return affected || !storage;
    });
    _data->textures.remove_if([&](const auto& entry) {
        return entry.texture.expired() || std::ranges::find(owners, entry.path) != owners.end();
    });
    _data->materials.remove_if([&](const auto& entry) {
        const auto storage = entry.storage.lock();
        return !storage || entry.path == path ||
               std::ranges::find(storage->data.source_files, path) != storage->data.source_files.end();
    });
}
Assets3D::Stats Assets3D::stats() const {
    Stats s = _data->counters;
    s.model_entries = _data->models.size();
    s.texture_entries = _data->textures.size();
    s.material_entries = _data->materials.size();
    s.shader_entries = _data->shaders.size();
    return s;
}

AssetHandle<Model3D> Assets3D::load_model(const std::filesystem::path& path) {
    return load_model(path, MeshOptions3D{});
}

const Diagnostics3D& Assets3D::diagnostics() const {
    return _data->errors;
}

AssetHandle<Model3D> Assets3D::load_model(const std::filesystem::path& path, const MeshOptions3D& options) {
    _data->errors.clear();
    std::string why;
    auto result = load_model_impl(path, options, &why);
    if (!result)
        fail3d(_data->errors, "Assets3D::load_model(" + path.string() + ")", why.empty() ? "model load failed" : why);
    return result;
}
Texture Assets3D::load_texture(const std::filesystem::path& path, bool nearest, bool srgb) {
    _data->errors.clear();
    std::string why;
    auto result = load_texture_impl(path, nearest, srgb, &why);
    if (!result)
        fail3d(_data->errors, "Assets3D::load_texture(" + path.string() + ")",
               why.empty() ? "texture load failed" : why);
    return result;
}
MaterialShader3D Assets3D::load_material_shader(const std::filesystem::path& path) {
    _data->errors.clear();
    std::string why;
    auto result = load_material_shader_impl(path, &why);
    if (!result)
        fail3d(_data->errors, "Assets3D::load_material_shader(" + path.string() + ")",
               why.empty() ? "shader load failed" : why);
    return result;
}
AssetHandle<MaterialBinding3D> Assets3D::load_materials(const std::filesystem::path& path,
                                                        const std::shared_ptr<const Model3D>& model,
                                                        std::string_view variant) {
    _data->errors.clear();
    std::string why;
    auto result = load_materials_impl(path, model, variant, &why);
    if (!result)
        fail3d(_data->errors, "Assets3D::load_materials(" + path.string() + ")",
               why.empty() ? "material load failed" : why);
    return result;
}

AssetHandle<Model3D> Assets3D::load_model_impl(const std::filesystem::path& path, const MeshOptions3D& options,
                                               std::string* error) {
    auto& d = *_data;
    const auto resolved = (d.root / path).lexically_normal();
    trim();
    for (auto it = d.models.begin(); it != d.models.end(); ++it) {
        if (it->path == resolved && it->options == options) {
            if (auto storage = it->storage.lock()) {
                ++d.counters.model_hits;
                d.models.splice(d.models.end(), d.models, it);
                if (error)
                    error->clear();
                return AssetHandle<Model3D>{std::move(storage)};
            }
        }
    }
    ++d.counters.model_loads;
    auto storage = std::make_shared<AssetStorage<Model3D>>();
    if (d.renderer->available())
        storage->data = d.renderer->load_model_shared(resolved, options, error, this);
    else {
        std::string why;
        storage->data = cpu_model(resolved, why);
        if (!storage->data)
            KIN_LOG_WARN_F("render", "3D: model not loaded",
                           (LogFields{{.name = "path", .value = resolved.string()}, {.name = "reason", .value = why}}));
        if (error)
            *error = why;
    }
    if (!storage->data)
        return {};
    storage->is_loaded = true;
    if (d.limits.models) {
        while (d.models.size() >= d.limits.models)
            d.models.pop_front();
        d.models.push_back({resolved, options, storage});
    }
    return AssetHandle<Model3D>{std::move(storage)};
}

Texture Assets3D::load_texture_impl(const std::filesystem::path& path, bool nearest, bool srgb, std::string* error) {
    const auto resolved = (_data->root / path).lexically_normal();
    std::string why;
    auto result = share_texture(resolved, ~0u, nearest, srgb, [&] {
        Texture texture;
        Renderer2D& renderer = _data->renderer->renderer();
        std::string extension = resolved.extension().string();
        for (char& c : extension)
            c = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        if (extension == ".ktx2" || extension == ".dds") {
            // Compressed texels and their mip chain, uploaded as they are. The file's format says whether they are
            // colour: a texture asked for the other way is said once, as it loads, and sampled as stored.
            const auto bytes = read_content_file(resolved);
            const auto image =
                bytes ? decode_compressed_image({reinterpret_cast<const u8*>(bytes->data()), bytes->size()}, &why)
                      : std::nullopt;
            if (!bytes)
                why = "cannot read the file";
            if (image) {
                if (image->srgb() != srgb) {
                    KIN_LOG_WARN_F("render",
                                   srgb ? "3D: a colour texture's file holds linear (UNORM) texels: sampled as stored"
                                        : "3D: a data texture's file holds sRGB colour: sampled decoded",
                                   (LogFields{{.name = "path", .value = resolved.string()}}));
                }
                texture = renderer.create_texture_compressed(*image);
                if (!texture)
                    why = "its format cannot be sampled here";
            }
        } else {
            const auto bytes = read_content_file(resolved);
            if (!bytes)
                why = "cannot read the file";
            else {
                const auto image =
                    decode_image({reinterpret_cast<const u8*>(bytes->data()), bytes->size()}, resolved.string());
                if (!image.valid())
                    why = "invalid image data";
                else
                    texture = srgb ? renderer.create_texture_from_rgba(image.rgba.data(), image.size)
                                   : renderer.create_texture(image.size, TextureFormat::Rgba8, image.rgba.data());
            }
        }
        if (texture)
            renderer.set_scale_mode(texture, nearest ? ScaleMode::Nearest : ScaleMode::Mipmapped);
        return texture;
    });
    if (error)
        *error = result ? "" : "cannot decode/upload " + resolved.string() + (why.empty() ? "" : ": " + why);
    return result;
}

MaterialShader3D Assets3D::load_material_shader_impl(const std::filesystem::path& source, std::string* error) {
    auto& d = *_data;
    const auto path = (d.root / source).lexically_normal();
    trim();
    for (auto it = d.shaders.begin(); it != d.shaders.end(); ++it) {
        if (it->path == path) {
            if (auto shader = it->shader.lock()) {
                ++d.counters.shader_hits;
                d.shaders.splice(d.shaders.end(), d.shaders, it);
                if (error)
                    error->clear();
                return MaterialShader3D{std::move(shader)};
            }
        }
    }
    ++d.counters.shader_loads;
    const auto size = content_file_size(path);
    if (!size || *size < 20 || *size > 4 * 1024 * 1024 || *size % 4) {
        if (error)
            *error = "shader missing or invalid SPIR-V size (limit 4 MiB): " + path.string();
        return {};
    }
    const auto bytes = read_content_file(path);
    if (!bytes || bytes->size() != *size || static_cast<u8>((*bytes)[0]) != 3 || static_cast<u8>((*bytes)[1]) != 2 ||
        static_cast<u8>((*bytes)[2]) != 35 || static_cast<u8>((*bytes)[3]) != 7) {
        if (error)
            *error = "invalid SPIR-V: " + path.string();
        return {};
    }
    auto shader = d.renderer->available()
                      ? d.renderer->create_material_shader({reinterpret_cast<const u8*>(bytes->data()), bytes->size()})
                      : MaterialShader3D{std::make_shared<CpuMaterialShader>()};
    if (!shader) {
        if (error)
            *error = "cannot create material shader: " + path.string();
        return {};
    }
    if (d.limits.shaders) {
        while (d.shaders.size() >= d.limits.shaders)
            d.shaders.pop_front();
        d.shaders.push_back({path, shader.backend()});
    }
    if (error)
        error->clear();
    return shader;
}

AssetHandle<MaterialBinding3D> Assets3D::load_materials_impl(const std::filesystem::path& path,
                                                             const std::shared_ptr<const Model3D>& model,
                                                             std::string_view variant, std::string* error) {
    auto& d = *_data;
    const auto resolved = (d.root / path).lexically_normal();
    trim();
    for (auto it = d.materials.begin(); it != d.materials.end(); ++it) {
        if (it->path == resolved && it->variant == variant)
            if (auto storage = it->storage.lock()) {
                if (storage->data.model.lock() == model && model) {
                    ++d.counters.material_hits;
                    d.materials.splice(d.materials.end(), d.materials, it);
                    if (error)
                        error->clear();
                    return AssetHandle<MaterialBinding3D>{std::move(storage)};
                }
            }
    }
    ++d.counters.material_loads;
    try {
        auto loaded = load_materials3d_asset(resolved);
        if (!loaded.ok()) {
            if (error)
                *error = loaded.diagnostics.front();
            return {};
        }
        auto storage = std::make_shared<AssetStorage<MaterialBinding3D>>();
        storage->data = build_materials3d_binding(*loaded.asset, model, variant, *this);
        storage->is_loaded = true;
        if (d.limits.materials) {
            while (d.materials.size() >= d.limits.materials)
                d.materials.pop_front();
            d.materials.push_back({resolved, std::string(variant), storage});
        }
        if (error)
            error->clear();
        return AssetHandle<MaterialBinding3D>{std::move(storage)};
    } catch (const std::exception& failure) {
        if (error)
            *error = resolved.string() + ": " + failure.what();
    }
    return {};
}

Texture Assets3D::share_texture(const std::filesystem::path& source, u32 embedded_image, bool nearest, bool srgb,
                                const std::function<Texture()>& load) {
    auto& d = *_data;
    const auto path = source.lexically_normal();
    for (auto it = d.textures.begin(); it != d.textures.end();) {
        auto backend = it->texture.lock();
        if (!backend) {
            it = d.textures.erase(it);
            continue;
        }
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
        while (d.textures.size() >= d.limits.textures)
            d.textures.pop_front();
        d.textures.push_back({path, embedded_image, nearest, srgb, texture.backend()});
    }
    return texture;
}

} // namespace kin
