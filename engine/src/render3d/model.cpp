#include <kin/render3d/model.hpp>

#include <kin/assets/content.hpp>
#include <kin/assets/image.hpp>
#include <kin/platform/log.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <kin/renderer/renderer2d.hpp>

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <map>
#include <tuple>
#include <utility>

namespace kin {
namespace {

// cgltf reads the file's other files (buffers, images) through these: kin's
// content folder or pack, as the model itself.
cgltf_result read_file(const cgltf_memory_options*, const cgltf_file_options*, const char* path, cgltf_size* size,
                       void** data) {
    const std::optional<std::string> bytes = read_content_file(std::filesystem::path{path});
    if (!bytes) {
        return cgltf_result_file_not_found;
    }
    void* copy = std::malloc(bytes->size() ? bytes->size() : 1);
    if (!copy) {
        return cgltf_result_out_of_memory;
    }
    std::memcpy(copy, bytes->data(), bytes->size());
    *size = bytes->size();
    *data = copy;
    return cgltf_result_success;
}

void release_file(const cgltf_memory_options*, const cgltf_file_options*, void* data) {
    std::free(data);
}

const char* describe(cgltf_result result) {
    switch (result) {
    case cgltf_result_success: return "success";
    case cgltf_result_data_too_short: return "data too short";
    case cgltf_result_unknown_format: return "not glTF";
    case cgltf_result_invalid_json: return "invalid JSON";
    case cgltf_result_invalid_gltf: return "invalid glTF";
    case cgltf_result_invalid_options: return "invalid options";
    case cgltf_result_file_not_found: return "a file it names was not found";
    case cgltf_result_io_error: return "read error";
    case cgltf_result_out_of_memory: return "out of memory";
    case cgltf_result_legacy_gltf: return "glTF 1.0 (2.0 is read)";
    default: return "error";
    }
}

Color to_color(const cgltf_float* rgba) {
    return to_srgb(LinearColor{rgba[0], rgba[1], rgba[2], rgba[3]});
}

class Loader {
public:
    Loader(Renderer3D& r3d, std::filesystem::path path, const MeshOptions& options) : _r3d(r3d), _path(std::move(path)), _options(options) {}

    Model load(std::string& error) {
        const std::optional<std::string> bytes = read_content_file(_path);
        if (!bytes) {
            error = "cannot read " + _path.string();
            return {};
        }
        cgltf_options options{};
        options.file.read = read_file;
        options.file.release = release_file;
        cgltf_data* data = nullptr;
        cgltf_result result = cgltf_parse(&options, bytes->data(), bytes->size(), &data);
        const std::unique_ptr<cgltf_data, void (*)(cgltf_data*)> owned{data, cgltf_free};
        if (result == cgltf_result_success) {
            result = cgltf_load_buffers(&options, data, _path.string().c_str());
        }
        if (result == cgltf_result_success) {
            result = cgltf_validate(data);
        }
        if (result != cgltf_result_success) {
            error = _path.string() + ": " + describe(result);
            return {};
        }
        _data = data;
        Model model;
        // The default scene's nodes, else every node without a parent.
        const cgltf_scene* scene = data->scene ? data->scene : data->scenes_count ? &data->scenes[0] : nullptr;
        if (scene) {
            for (cgltf_size i = 0; i < scene->nodes_count; ++i) {
                add_node(scene->nodes[i], model);
            }
        } else {
            for (cgltf_size i = 0; i < data->nodes_count; ++i) {
                if (!data->nodes[i].parent) add_node(&data->nodes[i], model);
            }
        }
        if (model.parts.empty()) {
            error = _path.string() + ": no triangles";
            return {};
        }
        bool first = true;
        for (const ModelPart& part : model.parts) {
            const Bounds3& b = part.mesh.bounds();
            for (int i = 0; i < 8; ++i) {
                const Vec3f p = part.transform.apply({(i & 1) ? b.max.x : b.min.x, (i & 2) ? b.max.y : b.min.y,
                                                      (i & 4) ? b.max.z : b.min.z});
                model.bounds.min = first ? p : Vec3f{std::min(model.bounds.min.x, p.x), std::min(model.bounds.min.y, p.y),
                                                     std::min(model.bounds.min.z, p.z)};
                model.bounds.max = first ? p : Vec3f{std::max(model.bounds.max.x, p.x), std::max(model.bounds.max.y, p.y),
                                                     std::max(model.bounds.max.z, p.z)};
                first = false;
            }
        }
        return model;
    }

private:
    void add_node(const cgltf_node* node, Model& model) {
        if (node->mesh) {
            Mat4f world;
            cgltf_node_transform_world(node, world.m);
            for (cgltf_size p = 0; p < node->mesh->primitives_count; ++p) {
                const cgltf_primitive& primitive = node->mesh->primitives[p];
                if (primitive.type != cgltf_primitive_type_triangles) {
                    continue; // points and lines are not drawn
                }
                MeshData data = mesh_data(primitive);
                if (data.vertices.empty()) {
                    continue;
                }
                Mesh mesh = _r3d.create_mesh(data, _options);
                if (!mesh.valid()) {
                    continue;
                }
                const char* name = node->name ? node->name : node->mesh->name ? node->mesh->name : "";
                model.parts.push_back({name, std::move(mesh), material(primitive.material), world});
            }
        }
        for (cgltf_size i = 0; i < node->children_count; ++i) {
            add_node(node->children[i], model);
        }
    }

    static const cgltf_accessor* attribute(const cgltf_primitive& primitive, cgltf_attribute_type type) {
        for (cgltf_size i = 0; i < primitive.attributes_count; ++i) {
            if (primitive.attributes[i].type == type && primitive.attributes[i].index == 0) {
                return primitive.attributes[i].data;
            }
        }
        return nullptr;
    }

    static MeshData mesh_data(const cgltf_primitive& primitive) {
        MeshData data;
        const cgltf_accessor* positions = attribute(primitive, cgltf_attribute_type_position);
        if (!positions) {
            return data;
        }
        const cgltf_accessor* normals = attribute(primitive, cgltf_attribute_type_normal);
        const cgltf_accessor* uvs = attribute(primitive, cgltf_attribute_type_texcoord);
        const cgltf_accessor* colors = attribute(primitive, cgltf_attribute_type_color);
        data.vertices.resize(positions->count);
        for (cgltf_size i = 0; i < positions->count; ++i) {
            MeshVertex& v = data.vertices[i];
            cgltf_accessor_read_float(positions, i, &v.position.x, 3);
            if (normals) cgltf_accessor_read_float(normals, i, &v.normal.x, 3);
            if (uvs) cgltf_accessor_read_float(uvs, i, &v.uv.x, 2);
            if (colors) {
                cgltf_float c[4] = {1, 1, 1, 1};
                cgltf_accessor_read_float(colors, i, c, cgltf_num_components(colors->type));
                v.color = to_color(c); // glTF's vertex colours are linear
            }
        }
        if (primitive.indices) {
            data.indices.resize(primitive.indices->count);
            for (cgltf_size i = 0; i < primitive.indices->count; ++i) {
                data.indices[i] = static_cast<u32>(cgltf_accessor_read_index(primitive.indices, i));
            }
        }
        if (!normals) {
            // Flat: each triangle its own vertices, facing its own way.
            MeshData flat;
            const std::size_t count = data.indices.empty() ? data.vertices.size() : data.indices.size();
            for (std::size_t t = 0; t + 2 < count; t += 3) {
                MeshVertex v[3];
                for (int k = 0; k < 3; ++k) v[k] = data.vertices[data.indices.empty() ? t + k : data.indices[t + k]];
                const Vec3f n = normalize(cross(v[1].position - v[0].position, v[2].position - v[0].position));
                for (MeshVertex& vertex : v) {
                    vertex.normal = n;
                    flat.vertices.push_back(vertex);
                }
            }
            return flat;
        }
        for (MeshVertex& v : data.vertices) {
            v.normal = normalize(v.normal);
        }
        return data;
    }

    Material3D material(const cgltf_material* m) {
        Material3D out;
        if (!m) {
            return out;
        }
        if (m->has_pbr_metallic_roughness) {
            const cgltf_pbr_metallic_roughness& pbr = m->pbr_metallic_roughness;
            out.color = to_color(pbr.base_color_factor);
            out.texture = texture(pbr.base_color_texture.texture, /*srgb=*/true);
            out.metallic = pbr.metallic_factor;
            out.roughness = pbr.roughness_factor;
            out.metallic_roughness = texture(pbr.metallic_roughness_texture.texture, /*srgb=*/false);
        }
        out.normal = texture(m->normal_texture.texture, /*srgb=*/false);
        out.normal_scale = m->normal_texture.scale;
        out.occlusion = texture(m->occlusion_texture.texture, /*srgb=*/false);
        out.occlusion_strength = m->occlusion_texture.scale;
        out.emissive_texture = texture(m->emissive_texture.texture, /*srgb=*/true);
        // Emissive light as a share of the base colour: the strongest channel.
        f32 emissive = std::max({m->emissive_factor[0], m->emissive_factor[1], m->emissive_factor[2]});
        if (m->has_emissive_strength) {
            emissive *= m->emissive_strength.emissive_strength;
        }
        out.emissive = emissive;
        out.unlit = m->unlit;
        out.double_sided = m->double_sided;
        out.blend = m->alpha_mode == cgltf_alpha_mode_blend  ? Blend3D::Alpha
                  : m->alpha_mode == cgltf_alpha_mode_mask ? Blend3D::Mask
                                                           : Blend3D::Opaque;
        out.alpha_cutoff = m->alpha_cutoff;
        // One sampler's wrap modes for all its maps: the base colour's, else the first map's.
        const cgltf_texture* first = m->has_pbr_metallic_roughness ? m->pbr_metallic_roughness.base_color_texture.texture : nullptr;
        for (const cgltf_texture* t : {m->pbr_metallic_roughness.metallic_roughness_texture.texture, m->normal_texture.texture,
                                       m->occlusion_texture.texture, m->emissive_texture.texture}) {
            first = first ? first : t;
        }
        if (first) {
            const auto wrap = [](cgltf_int mode) {
                return mode == 33071 ? Wrap3D::Clamp : mode == 33648 ? Wrap3D::Mirror : Wrap3D::Repeat; // REPEAT (10497) the default
            };
            out.wrap_u = first->sampler ? wrap(first->sampler->wrap_s) : Wrap3D::Repeat;
            out.wrap_v = first->sampler ? wrap(first->sampler->wrap_t) : Wrap3D::Repeat;
        }
        return out;
    }

    // A material's texture: colours (`srgb`) are decoded to linear light as
    // they are sampled; data (metallic-roughness, normals, occlusion) is
    // the numbers it holds, which only a plain 8-bit format keeps.
    Texture texture(const cgltf_texture* t, bool srgb) {
        if (!t) {
            return {};
        }
        // Pixel art asks for its texels as they are (a NEAREST sampler);
        // anything else is mipmapped.
        return texture(t->image, t->sampler && t->sampler->mag_filter == 9728, srgb);
    }

    Texture texture(const cgltf_image* image, bool nearest, bool srgb) {
        if (!image) {
            return {};
        }
        const Key key{image, nearest, srgb};
        if (const auto found = _textures.find(key); found != _textures.end()) {
            return found->second;
        }
        Image decoded;
        if (image->buffer_view) {
            const auto* bytes = static_cast<const u8*>(cgltf_buffer_view_data(image->buffer_view));
            if (bytes) decoded = decode_image({bytes, image->buffer_view->size}, image->name ? image->name : "texture");
        } else if (image->uri && std::strncmp(image->uri, "data:", 5) != 0) {
            std::string uri = image->uri;
            cgltf_decode_uri(uri.data());
            uri.resize(std::strlen(uri.c_str()));
            if (const auto bytes = read_content_file(_path.parent_path() / uri)) {
                decoded = decode_image({reinterpret_cast<const u8*>(bytes->data()), bytes->size()}, uri);
            }
        }
        Texture texture;
        if (decoded.valid()) {
            texture = srgb ? _r3d.renderer().create_texture_from_rgba(decoded.rgba.data(), decoded.size)
                           : _r3d.renderer().create_texture(decoded.size, TextureFormat::Rgba8, decoded.rgba.data());
            if (texture) _r3d.renderer().set_scale_mode(texture, nearest ? ScaleMode::Nearest : ScaleMode::Mipmapped);
        } else {
            KIN_LOG_WARN_F("render", "3D: a model's texture not read",
                           (LogFields{{.name = "model", .value = _path.string()}, {.name = "image", .value = image->name ? image->name : ""}}));
        }
        _textures[key] = texture;
        return texture;
    }

    Renderer3D& _r3d;
    std::filesystem::path _path;
    MeshOptions _options;
    const cgltf_data* _data = nullptr;
    using Key = std::tuple<const cgltf_image*, bool, bool>; // image, nearest, sRGB
    std::map<Key, Texture> _textures;
};

} // namespace

Model Renderer3D::load_model(const std::filesystem::path& path, std::string* error) {
    return load_model(path, MeshOptions{}, error);
}

Model Renderer3D::load_model(const std::filesystem::path& path, const MeshOptions& options, std::string* error) {
    std::string why;
    Model model;
    if (!_backend) {
        why = "3D is not available";
    } else {
        model = Loader{*this, path, options}.load(why);
    }
    if (!model.valid()) {
        KIN_LOG_WARN_F("render", "3D: model not loaded", (LogFields{{.name = "path", .value = path.string()}, {.name = "reason", .value = why}}));
    }
    if (error) {
        *error = why;
    }
    return model;
}

void Scene3D::draw(const Model& model, const Mat4f& transform, Color tint) {
    for (const ModelPart& part : model.parts) {
        draw(part.mesh, transform * part.transform, part.material, tint);
    }
}

ModelFile::ModelFile(Renderer3D& renderer, std::filesystem::path path, const MeshOptions& options)
    : _renderer(&renderer), _path(std::move(path)), _options(options) {
    load();
}

bool ModelFile::load() {
    std::error_code ec;
    _time = std::filesystem::last_write_time(_path, ec);
    std::string error;
    Model model = _renderer->load_model(_path, _options, &error);
    if (!model.valid()) {
        _error = error;
        return false;
    }
    _model = std::move(model);
    _error.clear();
    return true;
}

bool ModelFile::poll() {
    std::error_code ec;
    const auto time = std::filesystem::last_write_time(_path, ec);
    if (ec || time == _time) {
        return false; // a pack's files never change
    }
    return load();
}

} // namespace kin
