#include "material_asset3d_impl.hpp"
#include "model_bounds3d.hpp"

#include <algorithm>
#include <cmath>
#include <kin/assets/content.hpp>
#include <kin/assets/image.hpp>
#include <kin/assets/texture_file.hpp>
#include <kin/platform/log.hpp>
#include <kin/render3d/assets3d.hpp>
#include <kin/render3d/material_asset3d.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <kin/renderer/shader_compiler.hpp>
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
    auto parsed = read_model_data(*bytes, path);
    if (!parsed) {
        why = diagnostics3d_text(parsed.diagnostics);
        return {};
    }
    ModelData3D data = std::move(*parsed);
    if (!data.asset_diagnostics.empty()) {
        why = diagnostics3d_text(data.asset_diagnostics);
        return {};
    }
    Model3D model;
    model.source_files.push_back(path.lexically_normal());
    detail::ModelBounds3D bounds;
    for (ModelPartData3D& part : data.parts) {
        const std::size_t vertices = part.mesh.vertices.size(),
                          indices = part.mesh.indices.empty() ? vertices : part.mesh.indices.size();
        if (!part.mesh.validate() || vertices > std::numeric_limits<u32>::max() / sizeof(MeshVertex3D) ||
            indices > std::numeric_limits<u32>::max() / 4) {
            why = "model mesh could not be uploaded: " + part.name;
            return {};
        }
        const Bounds3D b = part.mesh.bounds();
        bounds.add(b, part.transform);
        Mesh3D mesh{nullptr, static_cast<u32>(vertices),  static_cast<u32>(indices), b, {},
                    {},      part.mesh.skin_joint_count()};
        model.parts.push_back({std::move(part.name), std::move(mesh), Material3D{}, part.transform, part.skin,
                               part.node, part.node_offset});
    }
    model.bounds = bounds.bounds;
    model.skeletons = std::move(data.skeletons);
    model.clips = std::move(data.clips);
    model.node_skeleton = data.node_skeleton;
    if (model.parts.empty()) {
        why = path.string() + ": no triangles";
        return {};
    }
    return model;
}

// What a material names as its shader where nothing draws (a session without 3D): a handle, so that the material binds
// as it would with 3D, that no renderer is ever given.
struct CpuMaterialShader final : IMaterialShaderBackend3D {};
struct CpuSurfaceShader final : ISurfaceShaderBackend3D {};

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
    struct MapArrayEntry {
        std::vector<std::filesystem::path> paths;
        std::weak_ptr<ITextureBackend> texture;
    };
    std::list<MapArrayEntry> map_arrays;
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
    struct GraphEntry {
        std::filesystem::path path;
        std::weak_ptr<ISurfaceShaderBackend3D> surface;
        CompiledMaterialGraph3D graph;
    };
    std::list<GraphEntry> graphs;
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

Renderer3D& Assets3D::renderer() const {
    return *_data->renderer;
}

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
    _data->graphs.remove_if([&](const auto& entry) {
        return entry.path == path || entry.path.string() + ".spv" == path.string() || entry.surface.expired();
    });
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
    s.graph_entries = _data->graphs.size();
    return s;
}

Result3D<AssetHandle<Model3D>> Assets3D::load_model(const std::filesystem::path& path) {
    auto value = load_model_operation(path);
    return result3d(std::move(value), _data->errors);
}

AssetHandle<Model3D> Assets3D::load_model_operation(const std::filesystem::path& path) {
    return load_model_operation(path, MeshOptions3D{});
}

const Diagnostics3D& Assets3D::diagnostics() const {
    return _data->errors;
}

Result3D<AssetHandle<Model3D>> Assets3D::load_model(const std::filesystem::path& path, const MeshOptions3D& options) {
    auto value = load_model_operation(path, options);
    return result3d(std::move(value), _data->errors);
}

AssetHandle<Model3D> Assets3D::load_model_operation(const std::filesystem::path& path, const MeshOptions3D& options) {
    _data->errors.clear();
    std::string why;
    auto result = load_model_impl(path, options, &why);
    if (!result)
        fail3d(_data->errors, "Assets3D::load_model(" + path.string() + ")", why.empty() ? "model load failed" : why);
    return result;
}
namespace {
// `image` resampled (bilinearly, as a pixel's centre falls) to `size`.
Image resampled(const Image& image, Vec2i size) {
    if (image.size == size)
        return image;
    Image out{size, std::vector<u8>(size_t(size.x) * size_t(size.y) * 4)};
    const auto at = [&](i32 x, i32 y, int c) {
        x = std::clamp(x, 0, image.size.x - 1);
        y = std::clamp(y, 0, image.size.y - 1);
        return f32(image.rgba[(size_t(y) * size_t(image.size.x) + size_t(x)) * 4 + size_t(c)]);
    };
    for (i32 y = 0; y < size.y; ++y)
        for (i32 x = 0; x < size.x; ++x) {
            const f32 u = (f32(x) + 0.5f) * f32(image.size.x) / f32(size.x) - 0.5f;
            const f32 v = (f32(y) + 0.5f) * f32(image.size.y) / f32(size.y) - 0.5f;
            const i32 x0 = i32(std::floor(u)), y0 = i32(std::floor(v));
            const f32 fx = u - f32(x0), fy = v - f32(y0);
            for (int c = 0; c < 4; ++c) {
                const f32 top = at(x0, y0, c) * (1 - fx) + at(x0 + 1, y0, c) * fx;
                const f32 bottom = at(x0, y0 + 1, c) * (1 - fx) + at(x0 + 1, y0 + 1, c) * fx;
                out.rgba[(size_t(y) * size_t(size.x) + size_t(x)) * 4 + size_t(c)] =
                    u8(std::clamp(top * (1 - fy) + bottom * fy + 0.5f, 0.0f, 255.0f));
            }
        }
    return out;
}
} // namespace

Result3D<Texture> Assets3D::load_map_array(std::span<const std::filesystem::path> maps) {
    auto& d = *_data;
    const auto fail = [&](const std::string& why) {
        fail3d(d.errors, "Assets3D::load_map_array", why);
        return Result3D<Texture>{std::nullopt, d.errors};
    };
    d.errors.clear();
    if (maps.empty() || maps.size() > max_graph_map_layers3d)
        return fail("1 to " + std::to_string(max_graph_map_layers3d) + " maps");
    std::vector<std::filesystem::path> paths;
    for (const auto& m : maps)
        paths.push_back(m.empty() ? m : (d.root / m).lexically_normal());
    for (auto it = d.map_arrays.begin(); it != d.map_arrays.end();) {
        auto texture = it->texture.lock();
        if (!texture) {
            it = d.map_arrays.erase(it);
            continue;
        }
        if (it->paths == paths)
            return {Texture{std::move(texture)}, {}};
        ++it;
    }
    std::vector<Image> images;
    Vec2i size{1, 1};
    for (const auto& path : paths) {
        if (path.empty()) {
            images.push_back(Image{{1, 1}, {255, 255, 255, 255}});
            continue;
        }
        const auto bytes = read_content_file(path);
        Image image =
            bytes ? decode_image({reinterpret_cast<const u8*>(bytes->data()), bytes->size()}, path.string()) : Image{};
        if (!image.valid())
            return fail("cannot read " + path.string());
        size = {std::max(size.x, image.size.x), std::max(size.y, image.size.y)};
        images.push_back(std::move(image));
    }
    size = {std::min(size.x, 4096), std::min(size.y, 4096)};
    std::vector<const u8*> layers;
    for (auto& image : images) {
        image = resampled(image, size);
        layers.push_back(image.rgba.data());
    }
    Texture texture = d.renderer->create_map_array(size, layers);
    if (!texture)
        return fail("the map array cannot be made here (no surface shaders, or too large)");
    d.map_arrays.push_back({std::move(paths), texture.backend()});
    return {std::move(texture), {}};
}

Result3D<Texture> Assets3D::load_texture(const std::filesystem::path& path, bool nearest, bool srgb) {
    auto value = load_texture_operation(path, nearest, srgb);
    return result3d(std::move(value), _data->errors);
}

Texture Assets3D::load_texture_operation(const std::filesystem::path& path, bool nearest, bool srgb) {
    _data->errors.clear();
    std::string why;
    auto result = load_texture_impl(path, nearest, srgb, &why);
    if (!result)
        fail3d(_data->errors, "Assets3D::load_texture(" + path.string() + ")",
               why.empty() ? "texture load failed" : why);
    return result;
}
Result3D<MaterialShader3D> Assets3D::load_material_shader(const std::filesystem::path& path) {
    auto value = load_material_shader_operation(path);
    return result3d(std::move(value), _data->errors);
}

MaterialShader3D Assets3D::load_material_shader_operation(const std::filesystem::path& path) {
    _data->errors.clear();
    std::string why;
    auto result = load_material_shader_impl(path, &why);
    if (!result)
        fail3d(_data->errors, "Assets3D::load_material_shader(" + path.string() + ")",
               why.empty() ? "shader load failed" : why);
    return result;
}
Result3D<MaterialGraphShader3D> Assets3D::load_material_graph(const std::filesystem::path& path) {
    _data->errors.clear();
    std::string why;
    auto result = load_material_graph_impl(path, &why);
    if (!result.surface)
        fail3d(_data->errors, "Assets3D::load_material_graph(" + path.string() + ")",
               why.empty() ? "material graph load failed" : why);
    return result3d(std::move(result), _data->errors);
}

MaterialGraphShader3D Assets3D::load_material_graph_impl(const std::filesystem::path& source, std::string* error) {
    auto& d = *_data;
    const auto path = (d.root / source).lexically_normal();
    const auto fail = [&](std::string why) {
        if (error)
            *error = std::move(why);
        return MaterialGraphShader3D{};
    };
    for (auto it = d.graphs.begin(); it != d.graphs.end(); ++it)
        if (it->path == path)
            if (auto surface = it->surface.lock()) {
                ++d.counters.graph_hits;
                d.graphs.splice(d.graphs.end(), d.graphs, it);
                if (error)
                    error->clear();
                return {SurfaceShader3D{std::move(surface), it->graph.traits}, it->graph};
            }
    ++d.counters.graph_loads;
    const std::filesystem::path cooked_path = path.string() + ".spv";
    const auto cooked_size = content_file_size(cooked_path), source_size = content_file_size(path);
    // The cooked file when there is no source (a shipped game) or it is not older than the source.
    bool use_cooked = cooked_size.has_value();
    if (use_cooked && source_size) {
        std::error_code a, b;
        const auto cooked_time = std::filesystem::last_write_time(cooked_path, a);
        const auto source_time = std::filesystem::last_write_time(path, b);
        use_cooked = a || b || cooked_time >= source_time;
    }
    MaterialGraphShader3D made;
    if (use_cooked) {
        const auto bytes = read_content_file(cooked_path);
        std::string why;
        const auto cooked =
            bytes ? unpack_cooked_material_graph3d({reinterpret_cast<const u8*>(bytes->data()), bytes->size()}, &why)
                  : std::nullopt;
        if (!cooked)
            return fail(cooked_path.string() + ": " + (why.empty() ? "unreadable" : why));
        if (d.renderer->available()) {
            auto r = d.renderer->create_material_graph(*cooked);
            if (!r)
                return fail(cooked_path.string() + ": the surface shader was refused");
            made = std::move(*r);
        } else {
            made = {SurfaceShader3D{std::make_shared<CpuSurfaceShader>(), cooked->graph.traits}, cooked->graph};
        }
    } else if (source_size) {
        const auto text = read_content_file(path);
        if (!text)
            return fail("cannot read " + path.string());
        if (!d.renderer->available()) {
            made.graph = compile_material_graph3d(*text);
            if (!made.graph.ok())
                return fail(path.string() + ": " + made.graph.errors.front().node + ": " +
                            made.graph.errors.front().message);
            made.surface = SurfaceShader3D{std::make_shared<CpuSurfaceShader>(), made.graph.traits};
        } else if (!shader_compiler_available()) {
            return fail(path.string() + ": not cooked (" + cooked_path.filename().string() +
                        ": kin_material_graph cook, kin_cook_material_graph) and no glslc to compile it");
        } else {
            auto r = d.renderer->compile_material_graph(path);
            if (!r)
                return fail(diagnostics3d_text(r.diagnostics));
            made = std::move(*r);
        }
    } else {
        return fail("no material graph " + path.string() + " (nor " + cooked_path.filename().string() + ")");
    }
    if (d.limits.shaders) {
        while (d.graphs.size() >= d.limits.shaders)
            d.graphs.pop_front();
        d.graphs.push_back({path, made.surface.backend(), made.graph});
    }
    if (error)
        error->clear();
    return made;
}

Result3D<AssetHandle<MaterialBinding3D>> Assets3D::load_materials(const std::filesystem::path& path,
                                                                  const std::shared_ptr<const Model3D>& model,
                                                                  std::string_view variant) {
    auto value = load_materials_operation(path, model, variant);
    return result3d(std::move(value), _data->errors);
}

AssetHandle<MaterialBinding3D> Assets3D::load_materials_operation(const std::filesystem::path& path,
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
                            .value_or({})
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
        storage->data = build_materials3d_binding(*loaded.value, model, variant, *this);
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
