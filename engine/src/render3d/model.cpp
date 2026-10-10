#include <kin/render3d/model.hpp>

#include <kin/assets/content.hpp>
#include <kin/assets/image.hpp>
#include <kin/platform/log.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <kin/renderer/renderer2d.hpp>

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
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

// The local placing of a node, as translation, rotation and scale (a matrix is taken apart).
JointPose local_pose(const cgltf_node* node) {
    JointPose p;
    if (node->has_matrix) {
        Mat4f m;
        std::memcpy(m.m, node->matrix, sizeof(m.m));
        p.translation = {m.m[12], m.m[13], m.m[14]};
        Vec3f c0{m.m[0], m.m[1], m.m[2]}, c1{m.m[4], m.m[5], m.m[6]}, c2{m.m[8], m.m[9], m.m[10]};
        Vec3f s{length(c0), length(c1), length(c2)};
        if (dot(c0, cross(c1, c2)) < 0.0f) {
            s.x = -s.x; // a mirror: carried in the scale
        }
        p.scale = s;
        if (s.x != 0.0f && s.y != 0.0f && s.z != 0.0f) {
            c0 = c0 * (1.0f / s.x);
            c1 = c1 * (1.0f / s.y);
            c2 = c2 * (1.0f / s.z);
            // A rotation matrix with these columns, to a quaternion.
            const f32 trace = c0.x + c1.y + c2.z;
            Quatf q;
            if (trace > 0.0f) {
                const f32 r = std::sqrt(1.0f + trace) * 2.0f;
                q = {(c1.z - c2.y) / r, (c2.x - c0.z) / r, (c0.y - c1.x) / r, 0.25f * r};
            } else if (c0.x > c1.y && c0.x > c2.z) {
                const f32 r = std::sqrt(1.0f + c0.x - c1.y - c2.z) * 2.0f;
                q = {0.25f * r, (c1.x + c0.y) / r, (c2.x + c0.z) / r, (c1.z - c2.y) / r};
            } else if (c1.y > c2.z) {
                const f32 r = std::sqrt(1.0f + c1.y - c0.x - c2.z) * 2.0f;
                q = {(c1.x + c0.y) / r, 0.25f * r, (c2.y + c1.z) / r, (c2.x - c0.z) / r};
            } else {
                const f32 r = std::sqrt(1.0f + c2.z - c0.x - c1.y) * 2.0f;
                q = {(c2.x + c0.z) / r, (c2.y + c1.z) / r, 0.25f * r, (c0.y - c1.x) / r};
            }
            p.rotation = q.normalized();
        }
        return p;
    }
    if (node->has_translation) p.translation = {node->translation[0], node->translation[1], node->translation[2]};
    if (node->has_rotation) p.rotation = Quatf{node->rotation[0], node->rotation[1], node->rotation[2], node->rotation[3]}.normalized();
    if (node->has_scale) p.scale = {node->scale[0], node->scale[1], node->scale[2]};
    return p;
}

Mat4f world_matrix(const cgltf_node* node) {
    Mat4f world;
    cgltf_node_transform_world(node, world.m);
    return world;
}

// Reads a glTF's nodes, meshes, skins and animations into ModelData (no GPU): the loader for a renderer (Loader, below) and
// read_model_data share it. A primitive's triangles are handed to `sink` as they are read, so a big model is not held whole.
class GltfReader {
public:
    using Sink = std::function<void(ModelPartData&&, const cgltf_material*)>;

    GltfReader(const cgltf_data* data, std::string label) : _data(data), _label(std::move(label)) {}

    ModelData read(const Sink& sink) {
        _sink = &sink;
        build_skins();
        const cgltf_scene* scene = _data->scene ? _data->scene : _data->scenes_count ? &_data->scenes[0] : nullptr;
        if (scene) {
            for (cgltf_size i = 0; i < scene->nodes_count; ++i) {
                add_node(scene->nodes[i]);
            }
        } else {
            for (cgltf_size i = 0; i < _data->nodes_count; ++i) {
                if (!_data->nodes[i].parent) add_node(&_data->nodes[i]);
            }
        }
        build_clips();
        return std::move(_out);
    }

private:
    struct SkinInfo {
        bool ok = false;
        u32 skeleton = 0;
        std::vector<u32> remap; // the file's joint index to the skeleton's
    };

    void note(const char* what, const std::string& detail) const {
        KIN_LOG_WARN_F("render", what, (LogFields{{.name = "model", .value = _label}, {.name = "detail", .value = detail}}));
    }

    // --- skins
    void build_skins() {
        _skins.assign(_data->skins_count, {});
        for (cgltf_size si = 0; si < _data->skins_count; ++si) {
            const cgltf_skin& skin = _data->skins[si];
            const std::size_t n = skin.joints_count;
            if (n == 0 || n > max_skin_joints) {
                note("3D: a skin is not used (its meshes are drawn static)",
                     std::string{skin.name ? skin.name : "skin"} + " has " + std::to_string(n) + " joints, " +
                         (n == 0 ? "none" : "more than the " + std::to_string(max_skin_joints) + " a skin can have"));
                continue;
            }
            // The joint above each, among the skin's own: the nearest ancestor node that is one.
            std::map<const cgltf_node*, std::size_t> index;
            for (std::size_t j = 0; j < n; ++j) {
                index[skin.joints[j]] = j;
            }
            std::vector<i32> parent_of(n, -1);
            bool skipped_nodes = false;
            for (std::size_t j = 0; j < n; ++j) {
                for (const cgltf_node* up = skin.joints[j]->parent; up; up = up->parent) {
                    const auto found = index.find(up);
                    if (found != index.end()) {
                        parent_of[j] = static_cast<i32>(found->second);
                        skipped_nodes = skipped_nodes || skin.joints[j]->parent != up;
                        break;
                    }
                }
            }
            if (skipped_nodes) {
                note("3D: a skin has non-joint nodes between joints: their placing is ignored", skin.name ? skin.name : "skin");
            }
            // Parents first (the joints of a file come in any order); the vertices' joint indices follow the file's, so a
            // table takes them over.
            std::vector<u32> order;
            std::vector<u8> placed(n, 0);
            order.reserve(n);
            while (order.size() < n) {
                const std::size_t before = order.size();
                for (std::size_t j = 0; j < n; ++j) {
                    if (!placed[j] && (parent_of[j] < 0 || placed[static_cast<std::size_t>(parent_of[j])])) {
                        placed[j] = 1;
                        order.push_back(static_cast<u32>(j));
                    }
                }
                if (order.size() == before) {
                    break; // a cycle: cgltf's validation does not allow it, but do not loop
                }
            }
            if (order.size() != n) {
                note("3D: a skin is not used (its joints form a loop)", skin.name ? skin.name : "skin");
                continue;
            }
            SkinInfo info;
            info.remap.assign(n, 0);
            for (std::size_t k = 0; k < n; ++k) {
                info.remap[order[k]] = static_cast<u32>(k);
            }
            Skeleton sk;
            sk.names.resize(n);
            sk.parents.resize(n);
            sk.rest.resize(n);
            sk.inverse_bind.assign(n, Mat4f{});
            for (std::size_t k = 0; k < n; ++k) {
                const std::size_t j = order[k];
                const cgltf_node* node = skin.joints[j];
                sk.names[k] = node->name ? node->name : "joint" + std::to_string(j);
                sk.parents[k] = parent_of[j] < 0 ? -1 : static_cast<i32>(info.remap[static_cast<std::size_t>(parent_of[j])]);
                sk.rest[k] = local_pose(node);
                if (skin.inverse_bind_matrices && j < skin.inverse_bind_matrices->count) {
                    cgltf_accessor_read_float(skin.inverse_bind_matrices, j, sk.inverse_bind[k].m, 16);
                }
                if (!_joint_of.count(node)) {
                    _joint_of[node] = {static_cast<i32>(si), static_cast<i32>(k)};
                }
            }
            // Where the roots hang: the nodes above them (the first root's parent; the others should agree).
            bool first_root = true;
            for (std::size_t k = 0; k < n; ++k) {
                if (sk.parents[k] >= 0) continue;
                const cgltf_node* above = skin.joints[order[k]]->parent;
                const Mat4f world = above ? world_matrix(above) : Mat4f{};
                if (first_root) {
                    sk.root_transform = world;
                    first_root = false;
                } else if (std::memcmp(world.m, sk.root_transform.m, sizeof(world.m)) != 0) {
                    note("3D: a skin's roots hang from different places: the first root's is used for all", skin.name ? skin.name : "skin");
                }
            }
            info.ok = true;
            info.skeleton = static_cast<u32>(_out.skeletons.size());
            _out.skeletons.push_back(std::move(sk));
            _skins[si] = std::move(info);
        }
    }

    // --- nodes
    void add_node(const cgltf_node* node) {
        if (node->mesh) {
            const Mat4f world = world_matrix(node);
            const char* name = node->name ? node->name : node->mesh->name ? node->mesh->name : "";
            // The skin of the mesh: its own, else the joint it hangs from (a prop on a hand), else none.
            const cgltf_skin* own = node->skin;
            const SkinInfo* own_info = own ? &_skins[static_cast<std::size_t>(own - _data->skins)] : nullptr;
            std::pair<i32, i32> attached{-1, -1};
            if (!own) {
                for (const cgltf_node* up = node; up && attached.first < 0; up = up->parent) {
                    const auto found = _joint_of.find(up);
                    if (found != _joint_of.end()) attached = found->second;
                }
            }
            for (cgltf_size p = 0; p < node->mesh->primitives_count; ++p) {
                const cgltf_primitive& primitive = node->mesh->primitives[p];
                if (primitive.type != cgltf_primitive_type_triangles) {
                    continue; // points and lines are not drawn
                }
                const bool bound = own_info && own_info->ok && attribute(primitive, cgltf_attribute_type_joints) &&
                                   attribute(primitive, cgltf_attribute_type_weights);
                MeshData data = mesh_data(primitive, bound ? own_info : nullptr, name);
                if (data.vertices.empty()) {
                    continue;
                }
                ModelPartData part{name, {}, world, -1};
                if (bound) {
                    part.transform = Mat4f{}; // the node's placing is ignored for a skinned mesh
                    part.skin = static_cast<i32>(own_info->skeleton);
                    part.mesh = std::move(data);
                } else if (attached.first >= 0 && _skins[static_cast<std::size_t>(attached.first)].ok) {
                    // Rigid on a joint: its vertices in the space the joints' inverse bind matrices start from (the world at
                    // rest), all of their weight on the joint.
                    const SkinInfo& info = _skins[static_cast<std::size_t>(attached.first)];
                    part.mesh.append(data, world);
                    part.mesh.skin.assign(part.mesh.vertices.size(), MeshSkinVertex{{static_cast<u8>(attached.second), 0, 0, 0}, {1.0f, 0.0f, 0.0f, 0.0f}});
                    part.transform = Mat4f{};
                    part.skin = static_cast<i32>(info.skeleton);
                } else {
                    part.mesh = std::move(data);
                }
                (*_sink)(std::move(part), primitive.material);
            }
        }
        for (cgltf_size i = 0; i < node->children_count; ++i) {
            add_node(node->children[i]);
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

    // JOINTS_0 and WEIGHTS_0 of a primitive as a skin stream (one entry a vertex of the accessors): the joints taken
    // through `info`'s table, the weights renormalised; a vertex with no usable influence goes to joint 0.
    void read_skin(const cgltf_primitive& primitive, const SkinInfo& info, std::size_t count, const char* name, std::vector<MeshSkinVertex>& out) const {
        const cgltf_accessor* joints = attribute(primitive, cgltf_attribute_type_joints);
        const cgltf_accessor* weights = attribute(primitive, cgltf_attribute_type_weights);
        out.assign(count, {});
        if (!joints || !weights || joints->count < count || weights->count < count) {
            return;
        }
        // Normalised integers are read as fractions; a file that forgot to say so is read as the spec means them.
        const f32 scale = weights->normalized ? 1.0f : weights->component_type == cgltf_component_type_r_8u ? 1.0f / 255.0f
                        : weights->component_type == cgltf_component_type_r_16u ? 1.0f / 65535.0f : 1.0f;
        std::size_t none = 0, bad = 0;
        for (std::size_t i = 0; i < count; ++i) {
            cgltf_uint j[4] = {0, 0, 0, 0};
            cgltf_float w[4] = {0, 0, 0, 0};
            cgltf_accessor_read_uint(joints, i, j, 4);
            cgltf_accessor_read_float(weights, i, w, 4);
            MeshSkinVertex v;
            f32 sum = 0.0f;
            for (std::size_t k = 0; k < 4; ++k) {
                f32 wk = w[k] * (weights->normalized ? 1.0f : scale);
                if (!std::isfinite(wk) || wk < 0.0f) wk = 0.0f;
                if (wk > 0.0f && j[k] >= info.remap.size()) {
                    ++bad; // an index past the skin's joints: that influence is dropped
                    wk = 0.0f;
                }
                v.joints[k] = wk > 0.0f ? static_cast<u8>(info.remap[j[k]]) : 0;
                v.weights[k] = wk;
                sum += wk;
            }
            if (sum > 0.0f) {
                for (f32& wk : v.weights) wk /= sum;
            } else {
                ++none;
                v.joints = {0, 0, 0, 0};
                v.weights = {1.0f, 0.0f, 0.0f, 0.0f};
            }
            out[i] = v;
        }
        if (none > 0 || bad > 0) {
            note("3D: a skinned mesh has vertices with no usable weights or joints past the skin: bound to joint 0 or dropped",
                 std::string{name} + ": " + std::to_string(none) + " vertices without weights, " + std::to_string(bad) + " influences past the joints");
        }
    }

    MeshData mesh_data(const cgltf_primitive& primitive, const SkinInfo* skin, const char* name) const {
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
        if (skin) {
            read_skin(primitive, *skin, data.vertices.size(), name, data.skin);
            if (attribute_index(primitive, cgltf_attribute_type_joints, 1)) {
                note("3D: a skinned mesh has more than four influences a vertex: the strongest four of JOINTS_0 are used", name);
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
                std::size_t at[3];
                for (int k = 0; k < 3; ++k) {
                    at[k] = data.indices.empty() ? t + static_cast<std::size_t>(k) : data.indices[t + static_cast<std::size_t>(k)];
                    v[k] = data.vertices[at[k]];
                }
                const Vec3f n = normalize(cross(v[1].position - v[0].position, v[2].position - v[0].position));
                for (int k = 0; k < 3; ++k) {
                    v[k].normal = n;
                    flat.vertices.push_back(v[k]);
                    if (skin) flat.skin.push_back(data.skin[at[k]]);
                }
            }
            return flat;
        }
        for (MeshVertex& v : data.vertices) {
            v.normal = normalize(v.normal);
        }
        return data;
    }

    static const cgltf_accessor* attribute_index(const cgltf_primitive& primitive, cgltf_attribute_type type, cgltf_int index) {
        for (cgltf_size i = 0; i < primitive.attributes_count; ++i) {
            if (primitive.attributes[i].type == type && primitive.attributes[i].index == index) {
                return primitive.attributes[i].data;
            }
        }
        return nullptr;
    }

    // --- animations
    void build_clips() {
        for (cgltf_size ai = 0; ai < _data->animations_count; ++ai) {
            const cgltf_animation& animation = _data->animations[ai];
            const std::string name = animation.name ? animation.name : "animation" + std::to_string(ai);
            // One clip a skeleton it moves, made when a channel first needs it.
            std::map<u32, std::size_t> clip_of;
            bool morph = false, other = false;
            for (cgltf_size ci = 0; ci < animation.channels_count; ++ci) {
                const cgltf_animation_channel& channel = animation.channels[ci];
                if (channel.target_path == cgltf_animation_path_type_weights) {
                    morph = true;
                    continue;
                }
                const auto found = channel.target_node ? _joint_of.find(channel.target_node) : _joint_of.end();
                if (found == _joint_of.end() || !_skins[static_cast<std::size_t>(found->second.first)].ok) {
                    other = true; // a node that is no joint: not a skeleton's to move
                    continue;
                }
                // The skeleton this joint belongs to is the first skin that has it.
                const SkinInfo& info = _skins[static_cast<std::size_t>(found->second.first)];
                const std::size_t joint = static_cast<std::size_t>(found->second.second);
                auto it = clip_of.find(info.skeleton);
                if (it == clip_of.end()) {
                    AnimClip3D clip;
                    clip.name = name;
                    clip.skeleton = info.skeleton;
                    clip.channels.resize(_out.skeletons[info.skeleton].joint_count());
                    _out.clips.push_back(std::move(clip));
                    it = clip_of.emplace(info.skeleton, _out.clips.size() - 1).first;
                }
                JointChannels& channels = _out.clips[it->second].channels[joint];
                read_channel(channel, channels);
            }
            for (const auto& [skeleton, at] : clip_of) {
                _out.clips[at].duration = _out.clips[at].key_duration();
            }
            if (clip_of.empty() && !morph) {
                note("3D: an animation moves no joint of a skin and is left out", name);
            }
            if (morph) {
                note("3D: morph target weights are not supported: those channels are ignored", name);
            }
            if (other && !clip_of.empty()) {
                note("3D: an animation moves nodes that are no joints: those channels are ignored", name);
            }
        }
    }

    void read_channel(const cgltf_animation_channel& channel, JointChannels& out) const {
        const cgltf_animation_sampler& sampler = *channel.sampler;
        const Interpolation3D mode = sampler.interpolation == cgltf_interpolation_type_step           ? Interpolation3D::Step
                                   : sampler.interpolation == cgltf_interpolation_type_cubic_spline ? Interpolation3D::CubicSpline
                                                                                                     : Interpolation3D::Linear;
        const std::size_t keys = sampler.input->count;
        const std::size_t per_key = mode == Interpolation3D::CubicSpline ? 3 : 1;
        if (sampler.output->count < keys * per_key) {
            note("3D: an animation channel has fewer values than keys: ignored", _label);
            return;
        }
        std::vector<f32> times(keys);
        for (std::size_t k = 0; k < keys; ++k) {
            cgltf_accessor_read_float(sampler.input, k, &times[k], 1);
        }
        const std::size_t values = keys * per_key;
        if (channel.target_path == cgltf_animation_path_type_rotation) {
            Track3D<Quatf>& track = out.rotation;
            track.interpolation = mode;
            track.times = std::move(times);
            track.values.resize(values);
            for (std::size_t k = 0; k < values; ++k) {
                f32 q[4] = {0, 0, 0, 1};
                cgltf_accessor_read_float(sampler.output, k, q, 4);
                track.values[k] = Quatf{q[0], q[1], q[2], q[3]};
            }
        } else {
            Track3D<Vec3f>& track = channel.target_path == cgltf_animation_path_type_translation ? out.translation : out.scale;
            track.interpolation = mode;
            track.times = std::move(times);
            track.values.resize(values);
            for (std::size_t k = 0; k < values; ++k) {
                f32 v[3] = {0, 0, 0};
                cgltf_accessor_read_float(sampler.output, k, v, 3);
                track.values[k] = {v[0], v[1], v[2]};
            }
        }
    }

    const cgltf_data* _data;
    std::string _label;
    const Sink* _sink = nullptr;
    ModelData _out;
    std::vector<SkinInfo> _skins;
    std::map<const cgltf_node*, std::pair<i32, i32>> _joint_of; // a joint node: its skin (the file's) and its index in it
};

// Parses glTF bytes (and loads the buffers they name) into cgltf's data; null with `error` set when it does not read.
std::unique_ptr<cgltf_data, void (*)(cgltf_data*)> parse_gltf(std::string_view bytes, const std::filesystem::path& path, std::string& error) {
    cgltf_options options{};
    options.file.read = read_file;
    options.file.release = release_file;
    cgltf_data* data = nullptr;
    cgltf_result result = cgltf_parse(&options, bytes.data(), bytes.size(), &data);
    std::unique_ptr<cgltf_data, void (*)(cgltf_data*)> owned{data, cgltf_free};
    if (result == cgltf_result_success) {
        result = cgltf_load_buffers(&options, data, path.string().c_str());
    }
    if (result == cgltf_result_success) {
        result = cgltf_validate(data);
    }
    if (result != cgltf_result_success) {
        error = path.string() + ": " + describe(result);
        owned.reset();
    }
    return owned;
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
        const auto owned = parse_gltf(*bytes, _path, error);
        if (!owned) {
            return {};
        }
        _data = owned.get();
        Model model;
        GltfReader reader{_data, _path.string()};
        ModelData read = reader.read([&](ModelPartData&& part, const cgltf_material* material) {
            Mesh mesh = _r3d.create_mesh(part.mesh, _options);
            if (!mesh.valid()) {
                return;
            }
            model.parts.push_back({part.name, std::move(mesh), this->material(material), part.transform, part.skin});
        });
        model.skeletons = std::move(read.skeletons);
        model.clips = std::move(read.clips);
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

bool read_model_data(std::string_view bytes, const std::filesystem::path& path, ModelData& out, std::string* error) {
    std::string why;
    const auto owned = parse_gltf(bytes, path, why);
    if (!owned) {
        if (error) *error = why;
        return false;
    }
    ModelData parts;
    GltfReader reader{owned.get(), path.string()};
    out = reader.read([&](ModelPartData&& part, const cgltf_material*) { parts.parts.push_back(std::move(part)); });
    out.parts = std::move(parts.parts);
    return true;
}

const AnimClip3D* Model::find_clip(std::string_view name, u32 skeleton) const {
    for (const AnimClip3D& clip : clips) {
        if (clip.skeleton == skeleton && clip.name == name) {
            return &clip;
        }
    }
    return nullptr;
}

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
