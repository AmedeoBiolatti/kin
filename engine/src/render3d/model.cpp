#include "model_bounds3d.hpp"

#include <kin/assets/content.hpp>
#include <kin/assets/image.hpp>
#include <kin/assets/texture_file.hpp>
#include <kin/platform/log.hpp>
#include <kin/render3d/assets3d.hpp>
#include <kin/render3d/model3d.hpp>
#include <kin/render3d/model_geometry3d.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <kin/renderer/renderer2d.hpp>

#define CGLTF_IMPLEMENTATION
#include <algorithm>
#include <cgltf.h>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>

namespace kin {
namespace {

// cgltf only reads these buffers. Keep the content owners alive until its
// parsed data is released, including loose-file strings and mapped pack spans.
struct GltfFiles {
    std::vector<ContentBytes> contents;
};
cgltf_result read_file(const cgltf_memory_options*, const cgltf_file_options* options, const char* path,
                       cgltf_size* size, void** data) {
    auto bytes = view_content_file(std::filesystem::path{path});
    if (!bytes)
        return cgltf_result_file_not_found;
    auto& files = *static_cast<GltfFiles*>(options->user_data);
    files.contents.push_back(std::move(*bytes));
    const auto view = files.contents.back().bytes;
    *size = view.size();
    *data = const_cast<std::byte*>(view.data());
    return cgltf_result_success;
}
void release_file(const cgltf_memory_options*, const cgltf_file_options*, void*) {}
struct GltfFree {
    std::shared_ptr<GltfFiles> files;
    void operator()(cgltf_data* data) const { cgltf_free(data); }
};
using GltfData = std::unique_ptr<cgltf_data, GltfFree>;

const char* describe(cgltf_result result) {
    switch (result) {
    case cgltf_result_success:
        return "success";
    case cgltf_result_data_too_short:
        return "data too short";
    case cgltf_result_unknown_format:
        return "not glTF";
    case cgltf_result_invalid_json:
        return "invalid JSON";
    case cgltf_result_invalid_gltf:
        return "invalid glTF";
    case cgltf_result_invalid_options:
        return "invalid options";
    case cgltf_result_file_not_found:
        return "a file it names was not found";
    case cgltf_result_io_error:
        return "read error";
    case cgltf_result_out_of_memory:
        return "out of memory";
    case cgltf_result_legacy_gltf:
        return "glTF 1.0 (2.0 is read)";
    default:
        return "error";
    }
}

Color to_color(const cgltf_float* rgba) {
    return to_srgb(LinearColor{rgba[0], rgba[1], rgba[2], rgba[3]});
}

// The local placing of a node, as translation, rotation and scale (a matrix is taken apart).
JointPose3D local_pose(const cgltf_node* node) {
    JointPose3D p;
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
    if (node->has_translation)
        p.translation = {node->translation[0], node->translation[1], node->translation[2]};
    if (node->has_rotation)
        p.rotation = Quatf{node->rotation[0], node->rotation[1], node->rotation[2], node->rotation[3]}.normalized();
    if (node->has_scale)
        p.scale = {node->scale[0], node->scale[1], node->scale[2]};
    return p;
}

Mat4f world_matrix(const cgltf_node* node) {
    Mat4f world;
    cgltf_node_transform_world(node, world.m);
    return world;
}

// How deep nodes may nest: the reader walks them by recursion, so a file may go no deeper (the collision reader's
// bound). A chain of the 256 joints a skin may have, under an armature, is legitimate, so the bound sits well above it.
constexpr std::size_t max_node_depth = 512;

// A path as kin's content functions compare paths: absolute and lexically normal, a folder without its trailing
// separator.
std::filesystem::path normalized_path(const std::filesystem::path& path) {
    std::error_code error;
    const std::filesystem::path full = std::filesystem::absolute(path, error);
    std::filesystem::path result = (error ? path : full).lexically_normal();
    if (!result.has_filename() && result.has_relative_path()) {
        result = result.parent_path();
    }
    return result;
}

// Whether `path` is `root` or below it, both as normalized_path makes them.
bool within(const std::filesystem::path& path, const std::filesystem::path& root) {
    const std::filesystem::path relative = path.lexically_relative(root);
    return !relative.empty() && *relative.begin() != "..";
}

// Whether a file the glTF at `model` names (its URI, not yet decoded) is out of bounds, and so not read: an absolute
// path, or, for a model in a mounted pack or under the content root, one whose ".." takes it out of there (a pack's own
// files stop at its mount point). A loose model elsewhere keeps "..": the developer chose that file and what lies about
// it.
bool outside_content(const std::filesystem::path& model, const char* uri) {
    std::string decoded = uri;
    cgltf_decode_uri(decoded.data());
    decoded.resize(std::strlen(decoded.c_str()));
    if (!decoded.empty() &&
        (decoded.front() == '/' || decoded.front() == '\\' || std::filesystem::path{decoded}.has_root_path())) {
        return true;
    }
    const std::filesystem::path from = normalized_path(model), file = normalized_path(model.parent_path() / decoded);
    if (const std::shared_ptr<const ContentPack> pack = content_pack_for(from)) {
        return content_pack_for(file) != pack;
    }
    if (const std::filesystem::path root = content_root(); !root.empty()) {
        const std::filesystem::path base = normalized_path(root);
        return within(from, base) && !within(file, base);
    }
    return false;
}

// The bytes of a base64 data: URI ("data:image/png;base64,..."), decoded as cgltf decodes a buffer's; empty when it is
// not one.
std::vector<u8> data_uri_bytes(const char* uri) {
    const char* comma = std::strchr(uri, ',');
    if (!comma || comma - uri < 7 || std::strncmp(comma - 7, ";base64", 7) != 0) {
        return {};
    }
    std::size_t length = std::strlen(comma + 1);
    while (length > 0 && comma[length] == '=') {
        --length; // padding carries no bits
    }
    const std::size_t size = length * 3 / 4; // six bits a character
    const cgltf_options options{};
    void* bytes = nullptr;
    if (size == 0 || cgltf_load_buffer_base64(&options, size, comma + 1, &bytes) != cgltf_result_success) {
        return {};
    }
    std::vector<u8> out(static_cast<const u8*>(bytes), static_cast<const u8*>(bytes) + size);
    std::free(bytes); // cgltf's default allocator is malloc
    return out;
}

// Why cgltf cannot read an accessor's elements one by one, or null. Its reads decode neither sparse accessors nor
// meshopt compression, and an accessor with no data (a Draco-compressed one, or one of zeros) reads as zeros.
const char* unreadable(const cgltf_accessor* accessor) {
    if (accessor->is_sparse) {
        return "a sparse accessor is not read";
    }
    if (accessor->buffer_view && accessor->buffer_view->has_meshopt_compression) {
        return "meshopt compression (EXT_meshopt_compression) is not read";
    }
    if (!accessor->buffer_view || !cgltf_buffer_view_data(accessor->buffer_view)) {
        return "an accessor has no data";
    }
    return nullptr;
}

// Reads a glTF's nodes, meshes, skins and animations into ModelData3D (no GPU): the loader for a renderer (Loader,
// below) and read_model_data share it. Each primitive is handed to `sink` as it is read. The GPU asset loader stages
// CPU parts until the complete profile is checked, then uploads and releases them.
class GltfReader {
public:
    using Sink = std::function<void(ModelPartData3D&&, const cgltf_material*)>;

    // `issues`, when given, collects unsupported content and unreadable primitives.
    GltfReader(const cgltf_data* data, std::string label, std::vector<std::string>* issues = nullptr)
        : _data(data), _label(std::move(label)), _issues(issues) {}

    ModelData3D read(const Sink& sink) {
        _sink = &sink;
        build_skins();
        if (_out.skeletons.size() > 1)
            unsupported("multiple distinct skin palettes are unsupported; export one palette with identical joint "
                        "order and inverse bind matrices, or split the model",
                        _label);
        const cgltf_scene* scene = _data->scene ? _data->scene : _data->scenes_count ? &_data->scenes[0] : nullptr;
        build_node_skeleton(scene);
        if (scene) {
            for (cgltf_size i = 0; i < scene->nodes_count; ++i) {
                add_node(scene->nodes[i]);
            }
        } else {
            for (cgltf_size i = 0; i < _data->nodes_count; ++i) {
                if (!_data->nodes[i].parent)
                    add_node(&_data->nodes[i]);
            }
        }
        if (_out.node_skeleton >= 0 && _node_parts == 0) {
            // No part hangs from the animated nodes (BrainStem's moves the node above its skin's joints, which carries
            // no mesh of its own): nothing for a pose of them to move, and the file reads as it did before nodes were
            // animated.
            _out.skeletons.pop_back();
            _out.node_skeleton = -1;
            _node_joint.clear();
        }
        build_clips();
        if (_out.node_skeleton > 0 && _node_parts > 0)
            unsupported("mixed skin and plain-node animation requires multiple poses; split the model", _label);
        return std::move(_out);
    }

private:
    struct SkinInfo {
        bool ok = false;
        u32 skeleton = 0;
        std::vector<u32> remap; // the file's joint index to the skeleton's
    };
    // What makes two of a file's skins the same skeleton: the joints, in the file's order, and their inverse bind
    // matrices (Blender writes one skin a mesh object over one armature: all equal).
    struct SkinKey {
        std::vector<const cgltf_node*> joints;
        std::vector<f32> binds;
        std::size_t skin = 0; // the file's index of the skin that was made
        bool operator==(const SkinKey& other) const { return joints == other.joints && binds == other.binds; }
    };

    void note(const char* what, const std::string& detail) const {
        KIN_LOG_WARN_F("render", what,
                       (LogFields{{.name = "model", .value = _label}, {.name = "detail", .value = detail}}));
    }
    void unsupported(const char* what, const std::string& detail) {
        note(what, detail);
        const std::string issue = std::string(what) + ": " + detail;
        _out.asset_diagnostics.push_back(issue);
        if (_issues)
            _issues->push_back(issue);
    }

    // --- skins
    void build_skins() {
        _skins.assign(_data->skins_count, {});
        for (cgltf_size si = 0; si < _data->skins_count; ++si) {
            const cgltf_skin& skin = _data->skins[si];
            const std::size_t n = skin.joints_count;
            if (n == 0 || n > max_skin_joints) {
                unsupported(
                    "model profile violation: unsupported skin; export at most 256 joints with readable MAT4 inverse "
                    "binds",
                    std::string{skin.name ? skin.name : "skin"} + " has " + std::to_string(n) + " joints, " +
                        (n == 0 ? "none" : "more than the " + std::to_string(max_skin_joints) + " a skin can have"));
                continue;
            }
            const cgltf_accessor* binds = skin.inverse_bind_matrices;
            if (const char* why = !binds ? nullptr : binds->type != cgltf_type_mat4 ? "not MAT4" : unreadable(binds)) {
                unsupported("model profile violation: unsupported skin; export at most 256 joints with readable MAT4 "
                            "inverse binds",
                            std::string{skin.name ? skin.name : "skin"} +
                                ": its inverse bind matrices cannot be read (" + why + ")");
                continue;
            }
            // The same joints with the same inverse bind matrices as an earlier skin: one skeleton, so that a character
            // exported as several mesh objects (each with its own skin) is posed whole. A skin that only shares some
            // joints keeps its own skeleton and receives the shared nodes' channels too.
            SkinKey key;
            key.joints.assign(skin.joints, skin.joints + n);
            key.binds.resize(n * 16, 0.0f);
            for (std::size_t j = 0; j < n; ++j) {
                for (u32 axis = 0; axis < 4; ++axis)
                    key.binds[j * 16 + axis * 5] = 1;
                if (binds && j < binds->count)
                    cgltf_accessor_read_float(binds, j, key.binds.data() + j * 16, 16);
            }
            key.skin = si;
            if (const auto same = std::find(_skin_keys.begin(), _skin_keys.end(), key); same != _skin_keys.end()) {
                _skins[si] = _skins[same->skin];
                continue;
            }
            // The joint above each, among the skin's own: the nearest ancestor node that is one. A plain node between
            // two joints (an offset an exporter left in) is kept as a joint that no vertex is bound to, so the joints
            // below it rest where their inverse bind matrices were made. Its placing cannot go into the child's rest
            // pose instead: a clip's track replaces all of a joint's placing.
            std::map<const cgltf_node*, std::size_t> index;
            for (std::size_t j = 0; j < n; ++j) {
                index[skin.joints[j]] = j;
            }
            std::vector<const cgltf_node*> nodes(skin.joints, skin.joints + n);
            for (std::size_t j = 0; j < n; ++j) {
                std::vector<const cgltf_node*> passed;
                for (const cgltf_node* up = skin.joints[j]->parent; up; up = up->parent) {
                    if (index.count(up)) {
                        for (const cgltf_node* between : passed) {
                            index[between] = nodes.size();
                            nodes.push_back(between);
                        }
                        break;
                    }
                    passed.push_back(up);
                }
            }
            if (nodes.size() > max_skin_joints) {
                unsupported("model profile violation: intermediary skin nodes exceed the joint limit; simplify the rig",
                            std::string{skin.name ? skin.name : "skin"} + " would have " +
                                std::to_string(nodes.size()) + " joints with them, more than the " +
                                std::to_string(max_skin_joints) + " a skin can have");
                for (std::size_t j = n; j < nodes.size(); ++j) {
                    index.erase(nodes[j]);
                }
                nodes.resize(n);
            }
            const std::size_t count = nodes.size();
            std::vector<i32> parent_of(count, -1);
            for (std::size_t j = 0; j < count; ++j) {
                for (const cgltf_node* up = nodes[j]->parent; up; up = up->parent) {
                    const auto found = index.find(up);
                    if (found != index.end()) {
                        parent_of[j] = static_cast<i32>(found->second);
                        break;
                    }
                }
            }
            // Parents first (the joints of a file come in any order); the vertices' joint indices follow the file's, so
            // a table takes them over.
            std::vector<u32> order;
            std::vector<u8> placed(count, 0);
            order.reserve(count);
            while (order.size() < count) {
                const std::size_t before = order.size();
                for (std::size_t j = 0; j < count; ++j) {
                    if (!placed[j] && (parent_of[j] < 0 || placed[static_cast<std::size_t>(parent_of[j])])) {
                        placed[j] = 1;
                        order.push_back(static_cast<u32>(j));
                    }
                }
                if (order.size() == before) {
                    break; // a cycle: cgltf's validation does not allow it, but do not loop
                }
            }
            if (order.size() != count) {
                unsupported("model profile violation: skin joints form a loop; export an acyclic rig",
                            skin.name ? skin.name : "skin");
                continue;
            }
            SkinInfo info;
            std::vector<u32> place(count, 0); // a node's index in the skeleton
            for (std::size_t k = 0; k < count; ++k) {
                place[order[k]] = static_cast<u32>(k);
            }
            info.remap.assign(place.begin(), place.begin() + static_cast<std::ptrdiff_t>(n));
            Skeleton3D sk;
            sk.names.resize(count);
            sk.parents.resize(count);
            sk.rest.resize(count);
            sk.inverse_bind.assign(count, Mat4f{});
            for (std::size_t k = 0; k < count; ++k) {
                const std::size_t j = order[k];
                const cgltf_node* node = nodes[j];
                sk.names[k] = node->name ? node->name
                              : j < n    ? "joint" + std::to_string(j)
                                         : "node" + std::to_string(node - _data->nodes);
                sk.parents[k] = parent_of[j] < 0 ? -1 : static_cast<i32>(place[static_cast<std::size_t>(parent_of[j])]);
                sk.rest[k] = local_pose(node);
                if (j >= n) {
                    // A node between joints has no inverse bind matrix: the one that makes its matrix the identity at
                    // rest (nothing is bound to it, but it must be finite).
                    const Mat4f inverse = world_matrix(node).inverse();
                    if (std::all_of(inverse.m, inverse.m + 16, [](f32 x) { return std::isfinite(x); })) {
                        sk.inverse_bind[k] = inverse;
                    }
                } else if (skin.inverse_bind_matrices && j < skin.inverse_bind_matrices->count) {
                    cgltf_accessor_read_float(skin.inverse_bind_matrices, j, sk.inverse_bind[k].m, 16);
                }
                if (!_joint_of.count(node)) {
                    _joint_of[node] = {static_cast<i32>(si), static_cast<i32>(k)};
                }
                _animation_joints[node].push_back({static_cast<u32>(_out.skeletons.size()), static_cast<u32>(k)});
            }
            // Where the roots hang: the nodes above them (the first root's parent; the others should agree).
            bool first_root = true;
            for (std::size_t k = 0; k < count; ++k) {
                if (sk.parents[k] >= 0)
                    continue;
                const cgltf_node* above = nodes[order[k]]->parent;
                const Mat4f world = above ? world_matrix(above) : Mat4f{};
                if (first_root) {
                    sk.root_transform = world;
                    first_root = false;
                } else if (std::memcmp(world.m, sk.root_transform.m, sizeof(world.m)) != 0) {
                    unsupported("model profile violation: skin roots use different spaces; export a common root",
                                skin.name ? skin.name : "skin");
                }
            }
            info.ok = true;
            info.skeleton = static_cast<u32>(_out.skeletons.size());
            _out.skeletons.push_back(std::move(sk));
            _skins[si] = std::move(info);
            _skin_keys.push_back(std::move(key));
        }
    }

    // --- animated nodes
    // The node skeleton: the plain nodes (no joint of a skin in use) that an animation moves in the scene read, and
    // every node above them, parents first. Made before the nodes are read, so that each part knows the joint it hangs
    // from.
    void build_node_skeleton(const cgltf_scene* scene) {
        const std::size_t count = _data->nodes_count;
        std::vector<u8> listed(count, scene ? 0 : 1), moved(count, 0);
        if (scene) {
            for (cgltf_size i = 0; i < scene->nodes_count; ++i)
                listed[static_cast<std::size_t>(scene->nodes[i] - _data->nodes)] = 1;
        }
        std::size_t marked = 0;
        for (cgltf_size ai = 0; ai < _data->animations_count; ++ai) {
            const cgltf_animation& animation = _data->animations[ai];
            for (cgltf_size ci = 0; ci < animation.channels_count; ++ci) {
                const cgltf_animation_channel& channel = animation.channels[ci];
                const cgltf_node* node = channel.target_node;
                if (!node || channel.target_path == cgltf_animation_path_type_weights || _joint_of.count(node)) {
                    continue; // a morph weight, or a skin's joint: its skeleton's to move
                }
                bool drawn =
                    false; // under a node of the scene read (a channel of another scene's nodes moves nothing here)
                for (const cgltf_node* up = node; up && !drawn; up = up->parent)
                    drawn = listed[static_cast<std::size_t>(up - _data->nodes)] != 0;
                // The node and those above it; a marked node's are marked already.
                for (const cgltf_node* up = node; drawn && up && !moved[static_cast<std::size_t>(up - _data->nodes)];
                     up = up->parent) {
                    moved[static_cast<std::size_t>(up - _data->nodes)] = 1;
                    ++marked;
                }
            }
        }
        if (marked == 0) {
            return;
        }
        if (marked > max_skin_joints) {
            unsupported("model profile violation: animated node hierarchy exceeds the joint limit; split the model",
                        std::to_string(marked) + " nodes with those above them, more than " +
                            std::to_string(max_skin_joints));
            return;
        }
        // Parents first: from each root down, in the file's order of children (marked nodes are closed upward, so a
        // node that is not marked has none below it).
        Skeleton3D sk;
        std::vector<const cgltf_node*> order;
        order.reserve(marked);
        const auto place = [&](auto&& self, const cgltf_node* node, i32 parent) -> void {
            const i32 joint = static_cast<i32>(order.size());
            order.push_back(node);
            sk.parents.push_back(parent);
            for (cgltf_size i = 0; i < node->children_count; ++i) {
                if (moved[static_cast<std::size_t>(node->children[i] - _data->nodes)])
                    self(self, node->children[i], joint);
            }
        };
        for (std::size_t i = 0; i < count; ++i) {
            if (moved[i] && !_data->nodes[i].parent)
                place(place, &_data->nodes[i], -1);
        }
        for (std::size_t k = 0; k < order.size(); ++k) {
            const cgltf_node* node = order[k];
            sk.names.push_back(node->name ? node->name : "node" + std::to_string(node - _data->nodes));
            sk.rest.push_back(local_pose(node));
            _node_joint[node] = static_cast<i32>(k);
        }
        // Nothing is bound to its joints: the identity inverse binds make its joint matrices its world matrices.
        sk.inverse_bind.assign(order.size(), Mat4f{});
        _out.node_skeleton = static_cast<i32>(_out.skeletons.size());
        _out.skeletons.push_back(std::move(sk));
    }

    // --- nodes
    void add_node(const cgltf_node* node) {
        if (node->mesh) {
            const Mat4f world = world_matrix(node);
            const char* name = node->name ? node->name : node->mesh->name ? node->mesh->name : "";
            // The node skeleton's joint the mesh hangs from (its own node or the nearest above it), and its placing
            // below it: the local placings on the way, multiplied as cgltf multiplies a world matrix.
            i32 hang = -1;
            Mat4f below{};
            for (const cgltf_node* up = node; up && !_node_joint.empty(); up = up->parent) {
                if (const auto found = _node_joint.find(up); found != _node_joint.end()) {
                    hang = found->second;
                    break;
                }
                Mat4f local;
                cgltf_node_transform_local(up, local.m);
                below = local * below;
            }
            // The skin of the mesh: its own, else the joint it hangs from (a prop on a hand), else none.
            const cgltf_skin* own = node->skin;
            const SkinInfo* own_info = own ? &_skins[static_cast<std::size_t>(own - _data->skins)] : nullptr;
            std::pair<i32, i32> attached{-1, -1};
            if (!own) {
                for (const cgltf_node* up = node; up && attached.first < 0; up = up->parent) {
                    const auto found = _joint_of.find(up);
                    if (found != _joint_of.end())
                        attached = found->second;
                }
            }
            for (cgltf_size p = 0; p < node->mesh->primitives_count; ++p) {
                const cgltf_primitive& primitive = node->mesh->primitives[p];
                if (primitive.type != cgltf_primitive_type_triangles) {
                    unsupported("only TRIANGLES primitives are supported; triangulate before export", name);
                    continue; // points and lines are not drawn
                }
                const bool bound = own_info && own_info->ok && attribute(primitive, cgltf_attribute_type_joints) &&
                                   attribute(primitive, cgltf_attribute_type_weights);
                if (own && !bound)
                    unsupported("skinned primitives require a supported skin, JOINTS_0 and WEIGHTS_0", name);
                MeshData3D data = mesh_data(primitive, bound ? own_info : nullptr, name);
                if (data.vertices.empty()) {
                    continue;
                }
                ModelPartData3D part{name, {}, world, -1};
                if (bound) {
                    part.transform = Mat4f{}; // the node's placing is ignored for a skinned mesh
                    part.skin = static_cast<i32>(own_info->skeleton);
                    part.mesh = std::move(data);
                } else if (attached.first >= 0 && _skins[static_cast<std::size_t>(attached.first)].ok) {
                    // Rigid on a joint: its vertices in the space the joints' inverse bind matrices start from (the
                    // world at rest), all of their weight on the joint.
                    const SkinInfo& info = _skins[static_cast<std::size_t>(attached.first)];
                    part.mesh.append(data, world);
                    part.mesh.skin.assign(
                        part.mesh.vertices.size(),
                        MeshSkinVertex3D{{static_cast<u8>(attached.second), 0, 0, 0}, {1.0f, 0.0f, 0.0f, 0.0f}});
                    part.transform = Mat4f{};
                    part.skin = static_cast<i32>(info.skeleton);
                } else {
                    part.mesh = std::move(data);
                    if (hang >= 0) {
                        part.node = hang;
                        part.node_offset = below;
                        ++_node_parts;
                    }
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
    // through `info`'s table, the weights renormalised; a vertex with no usable influence goes to joint 0. False when
    // cgltf cannot read them.
    bool read_skin(const cgltf_primitive& primitive, const SkinInfo& info, std::size_t count, const char* name,
                   std::vector<MeshSkinVertex3D>& out) {
        const cgltf_accessor* joints = attribute(primitive, cgltf_attribute_type_joints);
        const cgltf_accessor* weights = attribute(primitive, cgltf_attribute_type_weights);
        out.assign(count, {});
        if (!joints || !weights || joints->count < count || weights->count < count) {
            return true;
        }
        // Normalised integers are read as fractions; a file that forgot to say so is read as the spec means them.
        const f32 scale = weights->normalized                                     ? 1.0f
                          : weights->component_type == cgltf_component_type_r_8u  ? 1.0f / 255.0f
                          : weights->component_type == cgltf_component_type_r_16u ? 1.0f / 65535.0f
                                                                                  : 1.0f;
        std::size_t none = 0, bad = 0;
        for (std::size_t i = 0; i < count; ++i) {
            cgltf_uint j[4] = {0, 0, 0, 0};
            cgltf_float w[4] = {0, 0, 0, 0};
            if (!cgltf_accessor_read_uint(joints, i, j, 4) || !cgltf_accessor_read_float(weights, i, w, 4)) {
                return false;
            }
            MeshSkinVertex3D v;
            f32 sum = 0.0f;
            for (std::size_t k = 0; k < 4; ++k) {
                f32 wk = w[k] * (weights->normalized ? 1.0f : scale);
                if (!std::isfinite(wk) || wk < 0.0f)
                    wk = 0.0f;
                if (wk > 0.0f && j[k] >= info.remap.size()) {
                    ++bad; // an index past the skin's joints: that influence is dropped
                    wk = 0.0f;
                }
                v.joints[k] = wk > 0.0f ? static_cast<u8>(info.remap[j[k]]) : 0;
                v.weights[k] = wk;
                sum += wk;
            }
            if (sum > 0.0f) {
                for (f32& wk : v.weights)
                    wk /= sum;
            } else {
                ++none;
                v.joints = {0, 0, 0, 0};
                v.weights = {1.0f, 0.0f, 0.0f, 0.0f};
            }
            out[i] = v;
        }
        if (none > 0 || bad > 0) {
            unsupported("3D: model profile violation: skinned vertices require usable weights and valid joint indices",
                        std::string{name} + ": " + std::to_string(none) + " vertices without weights, " +
                            std::to_string(bad) + " influences past the joints");
        }
        return true;
    }

    // --- texture coordinates
    // A material's placing of its textures on the mesh: the TEXCOORD set they sample and the KHR_texture_transform
    // they apply to it. The mesh holds one set of texture coordinates, so the transform is applied to them as they are
    // read, which a material can only have when all of its textures agree on it.
    struct UvTransform {
        f32 offset[2] = {0.0f, 0.0f};
        f32 rotation = 0.0f;
        f32 scale[2] = {1.0f, 1.0f};
        cgltf_int texcoord = 0;

        bool operator==(const UvTransform& other) const {
            return offset[0] == other.offset[0] && offset[1] == other.offset[1] && rotation == other.rotation &&
                   scale[0] == other.scale[0] && scale[1] == other.scale[1] && texcoord == other.texcoord;
        }
        bool identity() const { return *this == UvTransform{.texcoord = texcoord}; }
        // As the extension's reference shader: scale, then rotate, then offset.
        Vec2f apply(Vec2f uv) const {
            const f32 c = std::cos(rotation), s = std::sin(rotation);
            const f32 u = uv.x * scale[0], v = uv.y * scale[1];
            return {c * u - s * v + offset[0], s * u + c * v + offset[1]};
        }
    };

    static UvTransform uv_transform_of(const cgltf_texture_view& view) {
        UvTransform t;
        t.texcoord = view.texcoord;
        if (view.has_transform) {
            t.offset[0] = view.transform.offset[0];
            t.offset[1] = view.transform.offset[1];
            t.rotation = view.transform.rotation;
            t.scale[0] = view.transform.scale[0];
            t.scale[1] = view.transform.scale[1];
            if (view.transform.has_texcoord) {
                t.texcoord = view.transform.texcoord;
            }
        }
        return t;
    }

    // The placing all of `m`'s textures share, or null for none to apply (no material, no textures, TEXCOORD_0 with no
    // transform, or textures that disagree: said once for the material, and its coordinates are read as they are).
    const UvTransform* uv_transform(const cgltf_material* m) {
        if (!m) {
            return nullptr;
        }
        const auto found = _uv_transforms.find(m);
        if (found != _uv_transforms.end()) {
            return found->second ? &*found->second : nullptr;
        }
        std::optional<UvTransform> shared;
        bool agree = true;
        const cgltf_texture_view* views[] = {
            m->has_pbr_metallic_roughness ? &m->pbr_metallic_roughness.base_color_texture : nullptr,
            m->has_pbr_metallic_roughness ? &m->pbr_metallic_roughness.metallic_roughness_texture : nullptr,
            &m->normal_texture, &m->occlusion_texture, &m->emissive_texture};
        for (const cgltf_texture_view* view : views) {
            if (!view || !view->texture) {
                continue;
            }
            const UvTransform t = uv_transform_of(*view);
            if (!shared) {
                shared = t;
            } else if (!(*shared == t)) {
                agree = false;
            }
        }
        if (!agree) {
            unsupported("3D: model profile violation: a material's textures require matching KHR_texture_transform and "
                        "TEXCOORD sets",
                        m->name ? m->name : "material");
            shared.reset();
        } else if (shared && shared->texcoord == 0 && shared->identity()) {
            shared.reset();
        }
        const auto& kept = _uv_transforms.emplace(m, shared).first->second;
        return kept ? &*kept : nullptr;
    }

    // Why a primitive's geometry cannot be read, or null. cgltf hands back zeros, or nothing, for compressed and sparse
    // data: read anyway, the mesh would be a heap of vertices at the origin. `uvs` is the texture coordinate set that
    // is read.
    static const char* refusal(const cgltf_primitive& primitive, const cgltf_accessor* uvs, bool skinned) {
        if (primitive.has_draco_mesh_compression) {
            return "Draco compression (KHR_draco_mesh_compression) is not read";
        }
        const cgltf_accessor* positions = attribute(primitive, cgltf_attribute_type_position);
        const cgltf_accessor* normals = attribute(primitive, cgltf_attribute_type_normal);
        const cgltf_accessor* colors = attribute(primitive, cgltf_attribute_type_color);
        if (positions->type != cgltf_type_vec3 || (normals && normals->type != cgltf_type_vec3) ||
            (uvs && uvs->type != cgltf_type_vec2) ||
            (colors && colors->type != cgltf_type_vec3 && colors->type != cgltf_type_vec4)) {
            return "an attribute has the wrong number of components (POSITION and NORMAL are VEC3, TEXCOORD VEC2, "
                   "COLOR_0 VEC3 or VEC4)";
        }
        const cgltf_accessor* used[] = {positions,
                                        normals,
                                        uvs,
                                        colors,
                                        primitive.indices,
                                        skinned ? attribute(primitive, cgltf_attribute_type_joints) : nullptr,
                                        skinned ? attribute(primitive, cgltf_attribute_type_weights) : nullptr};
        for (const cgltf_accessor* accessor : used) {
            if (const char* why = accessor ? unreadable(accessor) : nullptr) {
                return why;
            }
        }
        return nullptr;
    }

    // CPU inspection keeps partial data with diagnostics; usable GPU assets reject it.
    void refuse(const char* name, const std::string& why) {
        note("model profile violation: primitive geometry cannot be read", std::string{name} + ": " + why);
        _out.asset_diagnostics.push_back("model geometry could not be read: " + std::string{name} + ": " + why);
        if (_issues) {
            _issues->push_back("model geometry could not be read: " + std::string{name} + ": " + why);
        }
    }

    MeshData3D mesh_data(const cgltf_primitive& primitive, const SkinInfo* skin, const char* name) {
        MeshData3D data;
        if (primitive.targets_count)
            unsupported("morph target geometry is unsupported; bake the deformation before export", name);
        const cgltf_accessor* positions = attribute(primitive, cgltf_attribute_type_position);
        if (!positions) {
            return data;
        }
        // The texture coordinates the material samples: its set, transformed as it says, else TEXCOORD_0 as it is.
        const UvTransform* placing = uv_transform(primitive.material);
        const cgltf_accessor* uvs = attribute(primitive, cgltf_attribute_type_texcoord);
        if (!uvs && !placing && primitive.material) {
            const auto& m = *primitive.material;
            if (m.pbr_metallic_roughness.base_color_texture.texture ||
                m.pbr_metallic_roughness.metallic_roughness_texture.texture || m.normal_texture.texture ||
                m.occlusion_texture.texture || m.emissive_texture.texture)
                unsupported("textured primitives require their TEXCOORD set", name);
        }
        if (placing && placing->texcoord != 0) {
            if (const cgltf_accessor* set =
                    attribute_index(primitive, cgltf_attribute_type_texcoord, placing->texcoord)) {
                uvs = set;
            } else {
                unsupported("model profile violation: material requires a missing TEXCOORD set; export that UV set",
                            std::string{name} + ": TEXCOORD_" + std::to_string(placing->texcoord));
            }
        }
        if (const char* why = refusal(primitive, uvs, skin != nullptr)) {
            refuse(name, why);
            return data;
        }
        const cgltf_accessor* normals = attribute(primitive, cgltf_attribute_type_normal);
        const cgltf_accessor* colors = attribute(primitive, cgltf_attribute_type_color);
        data.vertices.resize(positions->count);
        bool read = true;
        for (cgltf_size i = 0; i < positions->count; ++i) {
            MeshVertex3D& v = data.vertices[i];
            read = cgltf_accessor_read_float(positions, i, &v.position.x, 3) && read;
            if (normals)
                read = cgltf_accessor_read_float(normals, i, &v.normal.x, 3) && read;
            if (uvs) {
                read = cgltf_accessor_read_float(uvs, i, &v.uv.x, 2) && read;
                if (placing) {
                    v.uv = placing->apply(v.uv);
                }
            }
            if (colors) {
                cgltf_float c[4] = {1, 1, 1, 1};
                read = cgltf_accessor_read_float(colors, i, c, cgltf_num_components(colors->type)) && read;
                v.color = to_color(c); // glTF's vertex colours are linear
            }
        }
        if (skin) {
            read = read_skin(primitive, *skin, data.vertices.size(), name, data.skin) && read;
            if (attribute_index(primitive, cgltf_attribute_type_joints, 1)) {
                unsupported("model profile violation: more than four skin influences per vertex; limit weights to four "
                            "before export",
                            name);
            }
        }
        // cgltf_accessor_read_index has no failure to report: refusal() has seen that the indices' data is there.
        if (primitive.indices) {
            data.indices.resize(primitive.indices->count);
            for (cgltf_size i = 0; i < primitive.indices->count; ++i) {
                data.indices[i] = static_cast<u32>(cgltf_accessor_read_index(primitive.indices, i));
            }
        }
        if (!read) {
            refuse(name, "cgltf could not read its values");
            return {};
        }
        if (!normals) {
            // Flat: each triangle its own vertices, facing its own way.
            MeshData3D flat;
            const std::size_t count = data.indices.empty() ? data.vertices.size() : data.indices.size();
            for (std::size_t t = 0; t + 2 < count; t += 3) {
                MeshVertex3D v[3];
                std::size_t at[3];
                for (int k = 0; k < 3; ++k) {
                    at[k] = data.indices.empty() ? t + static_cast<std::size_t>(k)
                                                 : data.indices[t + static_cast<std::size_t>(k)];
                    v[k] = data.vertices[at[k]];
                }
                const Vec3f n = normalize(cross(v[1].position - v[0].position, v[2].position - v[0].position));
                for (int k = 0; k < 3; ++k) {
                    v[k].normal = n;
                    flat.vertices.push_back(v[k]);
                    if (skin)
                        flat.skin.push_back(data.skin[at[k]]);
                }
            }
            return flat;
        }
        for (MeshVertex3D& v : data.vertices) {
            v.normal = normalize(v.normal);
        }
        return data;
    }

    static const cgltf_accessor* attribute_index(const cgltf_primitive& primitive, cgltf_attribute_type type,
                                                 cgltf_int index) {
        for (cgltf_size i = 0; i < primitive.attributes_count; ++i) {
            if (primitive.attributes[i].type == type && primitive.attributes[i].index == index) {
                return primitive.attributes[i].data;
            }
        }
        return nullptr;
    }

    // --- animations
    void build_clips() {
        // The node skeleton's clips go after every skin's, so that a file's skin clips keep the places they always had.
        std::vector<AnimClip3D> node_clips;
        for (cgltf_size ai = 0; ai < _data->animations_count; ++ai) {
            const cgltf_animation& animation = _data->animations[ai];
            const std::string name = animation.name ? animation.name : "animation" + std::to_string(ai);
            // One clip a skeleton it moves, made when a channel first needs it.
            std::map<u32, std::size_t> clip_of;
            AnimClip3D* node_clip = nullptr; // in node_clips
            bool morph = false, other = false;
            for (cgltf_size ci = 0; ci < animation.channels_count; ++ci) {
                const cgltf_animation_channel& channel = animation.channels[ci];
                if (channel.target_path == cgltf_animation_path_type_weights) {
                    morph = true;
                    continue;
                }
                const auto found =
                    channel.target_node ? _animation_joints.find(channel.target_node) : _animation_joints.end();
                if (found == _animation_joints.end()) {
                    // A node that is no joint: the node skeleton's, if it has it.
                    const auto plain = channel.target_node ? _node_joint.find(channel.target_node) : _node_joint.end();
                    if (plain == _node_joint.end()) {
                        other = true;
                        continue;
                    }
                    if (!node_clip) {
                        AnimClip3D clip;
                        clip.name = name;
                        clip.skeleton = static_cast<u32>(_out.node_skeleton);
                        clip.channels.resize(_out.skeletons[clip.skeleton].joint_count());
                        node_clip = &node_clips.emplace_back(std::move(clip));
                    }
                    read_channel(channel, node_clip->channels[static_cast<std::size_t>(plain->second)]);
                    continue;
                }
                // One glTF node can belong to several skins with different joint lists or inverse binds.
                for (const auto& [skeleton, joint] : found->second) {
                    auto it = clip_of.find(skeleton);
                    if (it == clip_of.end()) {
                        AnimClip3D clip;
                        clip.name = name;
                        clip.skeleton = skeleton;
                        clip.channels.resize(_out.skeletons[skeleton].joint_count());
                        _out.clips.push_back(std::move(clip));
                        it = clip_of.emplace(skeleton, _out.clips.size() - 1).first;
                    }
                    read_channel(channel, _out.clips[it->second].channels[joint]);
                }
            }
            for (const auto& [skeleton, at] : clip_of) {
                _out.clips[at].duration = _out.clips[at].key_duration();
            }
            if (node_clip) {
                node_clip->duration = node_clip->key_duration();
            }
            if (clip_of.empty() && !node_clip && !morph) {
                note("3D: an animation moves no joint of a skin and no node a part hangs from: it is left out", name);
            }
            if (morph) {
                unsupported("model profile violation: morph target animation is unsupported; bake it before export",
                            name);
            }
            if (other && (!clip_of.empty() || node_clip)) {
                note("3D: an animation moves nodes no part of the model hangs from: those channels are ignored", name);
            }
        }
        for (AnimClip3D& clip : node_clips) {
            _out.clips.push_back(std::move(clip));
        }
    }

    void read_channel(const cgltf_animation_channel& channel, JointChannels3D& out) {
        const cgltf_animation_sampler& sampler = *channel.sampler;
        const Interpolation3D mode = sampler.interpolation == cgltf_interpolation_type_step ? Interpolation3D::Step
                                     : sampler.interpolation == cgltf_interpolation_type_cubic_spline
                                         ? Interpolation3D::CubicSpline
                                         : Interpolation3D::Linear;
        const std::size_t keys = sampler.input->count;
        const std::size_t per_key = mode == Interpolation3D::CubicSpline ? 3 : 1;
        if (sampler.output->count < keys * per_key) {
            unsupported("model profile violation: animation channel has fewer values than keys", _label);
            return;
        }
        for (const cgltf_accessor* accessor : {sampler.input, sampler.output}) {
            if (const char* why = unreadable(accessor)) {
                unsupported("model profile violation: animation channel keys cannot be read", _label + ": " + why);
                return;
            }
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
            Track3D<Vec3f>& track =
                channel.target_path == cgltf_animation_path_type_translation ? out.translation : out.scale;
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
    std::vector<std::string>* _issues = nullptr;
    const Sink* _sink = nullptr;
    ModelData3D _out;
    std::vector<SkinInfo> _skins;
    std::map<const cgltf_node*, std::pair<i32, i32>>
        _joint_of; // a joint node: its skin (the file's) and its index in it
    std::map<const cgltf_node*, std::vector<std::pair<u32, u32>>> _animation_joints;
    std::map<const cgltf_node*, i32> _node_joint; // a node of the node skeleton: its joint
    std::size_t _node_parts = 0;                  // parts that hang from one
    std::vector<SkinKey> _skin_keys;              // the skins made, to fold the same again into them
    std::map<const cgltf_material*, std::optional<UvTransform>> _uv_transforms; // by material, once looked at
};

// Parses glTF bytes (and loads the buffers they name) into cgltf's data; null with `error` set when it does not read.
GltfData parse_gltf(std::string_view bytes, const std::filesystem::path& path, std::string& error) {
    auto files = std::make_shared<GltfFiles>();
    cgltf_options options{};
    options.file.user_data = files.get();
    options.file.read = read_file;
    options.file.release = release_file;
    cgltf_data* data = nullptr;
    cgltf_result result = cgltf_parse(&options, bytes.data(), bytes.size(), &data);
    GltfData owned{data, GltfFree{files}};
    // Refused before a buffer is read: nodes nested deeper than the reader may recurse, and a buffer outside the
    // model's content (outside_content; as a missing buffer would, the model does not load).
    std::string refused;
    if (result == cgltf_result_success) {
        for (cgltf_size i = 0; i < data->nodes_count && refused.empty(); ++i) {
            std::size_t depth = 0;
            for (const cgltf_node* up = &data->nodes[i]; up && depth <= max_node_depth; up = up->parent) {
                ++depth;
            }
            if (depth > max_node_depth) {
                refused = "its nodes nest more than " + std::to_string(max_node_depth) + " deep";
            }
        }
        for (cgltf_size i = 0; i < data->buffers_count && refused.empty(); ++i) {
            const char* uri = data->buffers[i].uri;
            if (uri && std::strncmp(uri, "data:", 5) != 0 && outside_content(path, uri)) {
                refused = std::string{"a buffer it names is outside its content (not read): "} + uri;
            }
        }
        // An extension the file says a reader must implement (extensionsRequired) that this one does not: refused
        // whole, rather than read as if it were not there. The ones read: unlit and emissive strength (materials),
        // quantized attributes (cgltf reads every component type), texture transforms (applied to the coordinates as
        // they are read), KTX2 textures (read where their format can be sampled, said as it fails otherwise).
        // Compressed geometry (Draco, meshopt) is not read, so a file that requires it is refused here too.
        static constexpr const char* implemented[] = {"KHR_materials_emissive_strength", "KHR_materials_unlit",
                                                      "KHR_mesh_quantization", "KHR_texture_basisu",
                                                      "KHR_texture_transform"};
        for (cgltf_size i = 0; i < data->extensions_required_count && refused.empty(); ++i) {
            const char* name = data->extensions_required[i];
            const bool known = name && std::any_of(std::begin(implemented), std::end(implemented),
                                                   [&](const char* it) { return std::strcmp(it, name) == 0; });
            if (!known) {
                refused = std::string{"it requires an extension kin does not implement: "} + (name ? name : "");
            }
        }
    }
    if (result == cgltf_result_success && refused.empty()) {
        result = cgltf_load_buffers(&options, data, path.string().c_str());
    }
    if (result == cgltf_result_success && refused.empty()) {
        result = cgltf_validate(data);
    }
    if (result != cgltf_result_success || !refused.empty()) {
        error = path.string() + ": " + (refused.empty() ? describe(result) : refused);
        owned.reset();
    }
    return owned;
}

class Loader {
public:
    using TextureResolver =
        std::function<Texture(const std::filesystem::path&, u32, bool, bool, const std::function<Texture()>&)>;
    Loader(Renderer3D& r3d, std::filesystem::path path, const MeshOptions3D& options, TextureResolver resolver = {})
        : _r3d(r3d), _path(std::move(path)), _options(options), _resolve_texture(std::move(resolver)) {}

    Model3D load(std::string& error) {
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
        Model3D model;
        model.source_files.push_back(_path.lexically_normal());
        const auto dependency = [&](const char* uri) {
            if (!uri || std::strncmp(uri, "data:", 5) == 0 || outside_content(_path, uri))
                return; // not read: see texture()
            std::string decoded = uri;
            cgltf_decode_uri(decoded.data());
            decoded.resize(std::strlen(decoded.c_str()));
            model.source_files.push_back((_path.parent_path() / decoded).lexically_normal());
        };
        for (cgltf_size i = 0; i < _data->buffers_count; ++i)
            dependency(_data->buffers[i].uri);
        for (cgltf_size i = 0; i < _data->images_count; ++i)
            dependency(_data->images[i].uri);
        std::ranges::sort(model.source_files);
        model.source_files.erase(std::unique(model.source_files.begin(), model.source_files.end()),
                                 model.source_files.end());
        GltfReader reader{_data, _path.string(), &_issues};
        struct Pending {
            ModelPartData3D part;
            const cgltf_material* material;
        };
        std::vector<Pending> pending;
        ModelData3D read = reader.read([&](ModelPartData3D&& part, const cgltf_material* material) {
            pending.push_back({std::move(part), material});
        });
        // Validate the complete asset before allocating any meshes or resolving materials.
        // In particular, a rejected last primitive must not upload earlier ones.
        if (!_issues.empty()) {
            error = diagnostics3d_text(_issues);
            return {};
        }
        if (pending.empty()) {
            error = _path.string() + ": no triangles";
            return {};
        }
        for (const auto& item : pending) {
            const auto valid = item.part.mesh.validate();
            if (!valid) {
                error = _path.string() + ": " + item.part.name + ": " + diagnostics3d_text(valid.diagnostics);
                return {};
            }
        }
        std::vector<Material3D> materials;
        materials.reserve(pending.size());
        for (const auto& item : pending)
            materials.push_back(this->material(item.material));
        if (!_issues.empty()) {
            error = diagnostics3d_text(_issues);
            return {};
        }
        for (std::size_t i = 0; i < pending.size(); ++i) {
            auto& item = pending[i];
            auto& part = item.part;
            auto mesh = _r3d.create_mesh(part.mesh, _options);
            if (!mesh) {
                error = _path.string() + ": " + part.name + ": " + diagnostics3d_text(mesh.diagnostics);
                return {};
            }
            model.parts.push_back({std::move(part.name), std::move(*mesh), std::move(materials[i]), part.transform,
                                   part.skin, part.node, part.node_offset});
            part.mesh = {};
        }
        model.skeletons = std::move(read.skeletons);
        model.clips = std::move(read.clips);
        model.node_skeleton = read.node_skeleton;
        detail::ModelBounds3D bounds;
        for (const ModelPart3D& part : model.parts) {
            const Bounds3D& b = part.mesh.bounds();
            bounds.add(b, part.transform);
        }
        model.bounds = bounds.bounds;
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
        const cgltf_texture* first =
            m->has_pbr_metallic_roughness ? m->pbr_metallic_roughness.base_color_texture.texture : nullptr;
        for (const cgltf_texture* t :
             {m->pbr_metallic_roughness.metallic_roughness_texture.texture, m->normal_texture.texture,
              m->occlusion_texture.texture, m->emissive_texture.texture}) {
            first = first ? first : t;
        }
        if (first) {
            const auto wrap = [](cgltf_int mode) {
                return mode == 33071   ? Wrap3D::Clamp
                       : mode == 33648 ? Wrap3D::Mirror
                                       : Wrap3D::Repeat; // REPEAT (10497) the default
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
        // anything else is mipmapped. KHR_texture_basisu's KTX2 is read only
        // where there is no other image (it needs a transcoder: said as it fails).
        return texture(t->image        ? t->image
                       : t->has_basisu ? t->basisu_image
                                       : nullptr,
                       t->sampler && t->sampler->mag_filter == 9728, srgb);
    }

    Texture texture(const cgltf_image* image, bool nearest, bool srgb) {
        if (!image) {
            return {};
        }
        const Key key{image, nearest, srgb};
        if (const auto found = _textures.find(key); found != _textures.end()) {
            return found->second;
        }
        // External files share across models; embedded images are namespaced by
        // their owning model and image index. Decode/upload only on a miss.
        std::filesystem::path source = _path;
        u32 embedded = static_cast<u32>(image - _data->images);
        bool outside = false;
        if (!image->buffer_view && image->uri && std::strncmp(image->uri, "data:", 5) != 0) {
            std::string uri = image->uri;
            cgltf_decode_uri(uri.data());
            uri.resize(std::strlen(uri.c_str()));
            source = _path.parent_path() / uri;
            embedded = ~0u;
            outside = outside_content(_path, image->uri);
            if (outside) {
                KIN_LOG_WARN_F(
                    "render", "3D: a model names a file outside its content: not read",
                    (LogFields{{.name = "model", .value = _path.string()}, {.name = "uri", .value = image->uri}}));
            }
        }
        const auto load = [&]() -> Texture {
            std::span<const u8> bytes;
            std::optional<std::string> file;
            std::vector<u8> inline_bytes;
            std::string name = image->name ? image->name : "texture";
            if (image->buffer_view) {
                const auto* data = static_cast<const u8*>(cgltf_buffer_view_data(image->buffer_view));
                if (data)
                    bytes = {data, image->buffer_view->size};
            } else if (image->uri && std::strncmp(image->uri, "data:", 5) != 0) {
                name = image->uri;
                cgltf_decode_uri(name.data());
                name.resize(std::strlen(name.c_str()));
                // A file outside the model's content is not read: the texture is missing.
                if (!outside)
                    file = read_content_file(_path.parent_path() / name);
                if (file)
                    bytes = {reinterpret_cast<const u8*>(file->data()), file->size()};
            } else if (image->uri) {
                // An image inside the file as a base64 data: URI (Blender's "glTF Embedded" export).
                inline_bytes = data_uri_bytes(image->uri);
                bytes = inline_bytes;
            }
            Texture texture;
            std::string why;
            if (is_compressed_image(bytes)) {
                // KTX2 (image/ktx2, as KHR_texture_basisu names it) or DDS: compressed texels and their mip chain,
                // uploaded as they are. The file's format says whether they are colour; a map that wants the other is
                // told once.
                if (const auto compressed = decode_compressed_image(bytes, &why)) {
                    if (compressed->srgb() != srgb) {
                        KIN_LOG_WARN_F(
                            "render",
                            srgb ? "3D: a model's colour texture holds linear (UNORM) texels: sampled as stored"
                                 : "3D: a model's data texture holds sRGB colour: sampled decoded",
                            (LogFields{{.name = "model", .value = _path.string()}, {.name = "image", .value = name}}));
                    }
                    texture = _r3d.renderer().create_texture_compressed(*compressed);
                    if (!texture)
                        why = "its format cannot be sampled here";
                }
            } else if (!bytes.empty()) {
                const Image decoded = decode_image(bytes, name);
                if (decoded.valid()) {
                    texture =
                        srgb ? _r3d.renderer().create_texture_from_rgba(decoded.rgba.data(), decoded.size)
                             : _r3d.renderer().create_texture(decoded.size, TextureFormat::Rgba8, decoded.rgba.data());
                }
            }
            if (texture) {
                _r3d.renderer().set_scale_mode(texture, nearest ? ScaleMode::Nearest : ScaleMode::Mipmapped);
            } else {
                LogFields fields{{.name = "model", .value = _path.string()},
                                 {.name = "image", .value = image->name ? image->name : ""}};
                if (!why.empty())
                    fields.push_back({.name = "reason", .value = why});
                KIN_LOG_WARN_F("render", "3D: a model's texture not read", (std::move(fields)));
            }
            return texture;
        };
        // A file outside the content is not looked for among the shared ones either.
        Texture texture =
            _resolve_texture && !outside ? _resolve_texture(source, embedded, nearest, srgb, load) : load();
        if (!texture)
            _issues.push_back("required model texture could not be decoded/uploaded: " + source.string());
        _textures[key] = texture;
        return texture;
    }

    Renderer3D& _r3d;
    std::filesystem::path _path;
    MeshOptions3D _options;
    TextureResolver _resolve_texture;
    const cgltf_data* _data = nullptr;
    std::vector<std::string> _issues;
    using Key = std::tuple<const cgltf_image*, bool, bool>; // image, nearest, sRGB
    std::map<Key, Texture> _textures;
};

} // namespace

static bool read_model_geometry3d_impl(const std::filesystem::path& path, ModelGeometry3D& out,
                                       ModelGeometryLimits3D limits, std::string* error) {
    const auto check = [](bool ok, const char* why) {
        if (!ok)
            throw std::runtime_error(why);
    };
    try {
        check(limits.vertices && limits.vertices <= UINT32_MAX && limits.triangles &&
                  limits.triangles <= UINT32_MAX / 3 && limits.parts,
              "invalid collision geometry budgets");
        const auto size = content_file_size(path);
        check(size && *size <= limits.source_bytes, "collision model source missing or exceeds byte budget");
        auto bytes = read_content_file(path);
        check(bytes && bytes->size() <= limits.source_bytes, "cannot read bounded collision model");
        struct Files {
            u64 remaining;
            std::vector<std::filesystem::path> sources;
            GltfFiles views;
        } files{limits.buffer_bytes, {path.lexically_normal()}, {}};
        cgltf_options options{};
        options.file.user_data = &files;
        options.file.read = [](const cgltf_memory_options* memory, const cgltf_file_options* options, const char* file,
                               cgltf_size* size, void** data) {
            auto& context = *static_cast<Files*>(options->user_data);
            const auto bytes = content_file_size(file);
            if (!bytes || *bytes > context.remaining)
                return cgltf_result_io_error;
            context.remaining -= *bytes;
            context.sources.push_back(std::filesystem::path(file).lexically_normal());
            auto borrowed = *options;
            borrowed.user_data = &context.views;
            return read_file(memory, &borrowed, file, size, data);
        };
        options.file.release = release_file;
        cgltf_data* data = nullptr;
        const auto result = cgltf_parse(&options, bytes->data(), bytes->size(), &data);
        std::unique_ptr<cgltf_data, void (*)(cgltf_data*)> owned{data, cgltf_free};
        check(result == cgltf_result_success, "invalid collision glTF");
        check(data->nodes_count <= 4096, "collision model node budget exceeded");
        u64 buffers = 0;
        for (cgltf_size i = 0; i < data->buffers_count; ++i) {
            check(data->buffers[i].size <= limits.buffer_bytes - buffers, "collision buffer byte budget exceeded");
            buffers += data->buffers[i].size;
            const char* uri = data->buffers[i].uri;
            check(!uri || std::strncmp(uri, "data:", 5) == 0 || !outside_content(path, uri),
                  "collision buffer outside the model's content");
        }
        for (cgltf_size i = 0; i < data->buffer_views_count; ++i)
            check(!data->buffer_views[i].has_meshopt_compression,
                  "compressed collision buffers unsupported; use a collision proxy");
        std::vector<const cgltf_node*> nodes;
        std::vector<bool> visited(data->nodes_count);
        const auto walk = [&](auto&& self, const cgltf_node* node, size_t depth) -> void {
            check(depth < 64, "collision node hierarchy too deep");
            const size_t index = node - data->nodes;
            check(index < data->nodes_count && !visited[index], "invalid/cyclic collision node hierarchy");
            visited[index] = true;
            nodes.push_back(node);
            for (cgltf_size i = 0; i < node->children_count; ++i)
                self(self, node->children[i], depth + 1);
        };
        const auto* scene = data->scene ? data->scene : data->scenes_count ? &data->scenes[0] : nullptr;
        if (scene)
            for (cgltf_size i = 0; i < scene->nodes_count; ++i)
                walk(walk, scene->nodes[i], 0);
        else
            for (cgltf_size i = 0; i < data->nodes_count; ++i)
                if (!data->nodes[i].parent)
                    walk(walk, &data->nodes[i], 0);
        size_t vertices = 0, triangles = 0, parts = 0;
        const auto positions = [](const cgltf_primitive& primitive) -> const cgltf_accessor* {
            for (cgltf_size i = 0; i < primitive.attributes_count; ++i)
                if (primitive.attributes[i].type == cgltf_attribute_type_position)
                    return primitive.attributes[i].data;
            return nullptr;
        };
        for (const auto* node : nodes)
            if (node->mesh) {
                check(!node->skin, "skinned collision models unsupported; use an explicit proxy");
                for (cgltf_size i = 0; i < node->mesh->primitives_count; ++i) {
                    const auto& p = node->mesh->primitives[i];
                    const auto* a = positions(p);
                    check(a && a->type == cgltf_type_vec3 && p.type == cgltf_primitive_type_triangles &&
                              !p.has_draco_mesh_compression,
                          "collision model requires uncompressed triangle positions");
                    const auto count = p.indices ? p.indices->count : a->count;
                    check(count % 3 == 0 && a->count <= limits.vertices - vertices &&
                              count / 3 <= limits.triangles - triangles && parts < limits.parts,
                          "collision geometry budget exceeded");
                    vertices += a->count;
                    triangles += count / 3;
                    ++parts;
                }
                // cgltf's world transform follows parents; check that path too.
                size_t depth = 0;
                for (auto* p = node; p; p = p->parent)
                    check(++depth <= 64, "cyclic collision parent chain");
            }
        check(parts && vertices && triangles, "collision model contains no triangles");
        check(cgltf_load_buffers(&options, data, path.string().c_str()) == cgltf_result_success,
              "collision buffers missing or exceed byte budget");
        check(cgltf_validate(data) == cgltf_result_success, "invalid collision geometry accessors");
        ModelGeometry3D geometry;
        geometry.vertices.reserve(vertices);
        geometry.indices.reserve(triangles * 3);
        for (const auto* node : nodes)
            if (node->mesh) {
                const auto transform = world_matrix(node);
                for (cgltf_size i = 0; i < node->mesh->primitives_count; ++i) {
                    const auto& p = node->mesh->primitives[i];
                    const auto* a = positions(p);
                    const u32 first = u32(geometry.vertices.size());
                    for (cgltf_size j = 0; j < a->count; ++j) {
                        float v[3];
                        check(cgltf_accessor_read_float(a, j, v, 3), "cannot read collision position");
                        const auto point = transform.apply({v[0], v[1], v[2]});
                        check(std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z),
                              "non-finite collision geometry");
                        geometry.vertices.push_back(point);
                    }
                    const auto count = p.indices ? p.indices->count : a->count;
                    for (cgltf_size j = 0; j < count; ++j) {
                        const auto index = p.indices ? cgltf_accessor_read_index(p.indices, j) : j;
                        check(index < a->count, "collision index out of range");
                        geometry.indices.push_back(first + u32(index));
                    }
                }
            }
        std::ranges::sort(files.sources);
        files.sources.erase(std::unique(files.sources.begin(), files.sources.end()), files.sources.end());
        geometry.source_files = std::move(files.sources);
        out = std::move(geometry);
        if (error)
            error->clear();
        return true;
    } catch (const std::exception& failure) {
        if (error)
            *error = path.string() + ": " + failure.what();
        return false;
    }
}
Result3D<ModelGeometry3D> read_model_geometry3d(const std::filesystem::path& path, ModelGeometryLimits3D limits) {
    ModelGeometry3D value;
    std::string why;
    if (!read_model_geometry3d_impl(path, value, limits, &why))
        return {std::nullopt, {"read_model_geometry3d: " + why}};
    return result3d(std::move(value));
}

static bool read_model_data_impl(std::string_view bytes, const std::filesystem::path& path, ModelData3D& out,
                                 std::string* error) {
    std::string why;
    const auto owned = parse_gltf(bytes, path, why);
    if (!owned) {
        if (error)
            *error = why;
        return false;
    }
    ModelData3D parts;
    GltfReader reader{owned.get(), path.string()};
    out = reader.read([&](ModelPartData3D&& part, const cgltf_material*) { parts.parts.push_back(std::move(part)); });
    out.parts = std::move(parts.parts);
    if (error)
        error->clear();
    return true;
}
Result3D<ModelData3D> read_model_data(std::string_view bytes, const std::filesystem::path& path) {
    ModelData3D value;
    std::string why;
    if (!read_model_data_impl(bytes, path, value, &why))
        return {std::nullopt, {"read_model_data: " + why}};
    return result3d(std::move(value));
}

const AnimClip3D* Model3D::find_clip(std::string_view name, u32 skeleton) const {
    for (const AnimClip3D& clip : clips) {
        if (clip.skeleton == skeleton && clip.name == name) {
            return &clip;
        }
    }
    return nullptr;
}

Mat4f Model3D::part_transform(std::size_t part, std::span<const Mat4f> node_matrices) const {
    if (part >= parts.size()) {
        return {};
    }
    const ModelPart3D& p = parts[part];
    if (p.node < 0 || static_cast<std::size_t>(p.node) >= node_matrices.size()) {
        return p.transform;
    }
    return node_matrices[static_cast<std::size_t>(p.node)] * p.node_offset;
}

Result3D<Model3D> Renderer3D::load_model(const std::filesystem::path& path) {
    auto value = load_model_operation(path);
    return result3d(std::move(value), _errors);
}

Model3D Renderer3D::load_model_operation(const std::filesystem::path& path) {
    return load_model_operation(path, MeshOptions3D{});
}

Result3D<Model3D> Renderer3D::load_model(const std::filesystem::path& path, const MeshOptions3D& options) {
    auto value = load_model_operation(path, options);
    return result3d(std::move(value), _errors);
}

Model3D Renderer3D::load_model_operation(const std::filesystem::path& path, const MeshOptions3D& options) {
    _errors.clear();
    std::string why;
    auto result = load_model_shared(path, options, &why, nullptr);
    if (!result) {
        fail3d(_errors, "Renderer3D::load_model(" + path.string() + ")", why.empty() ? "model load failed" : why);
        result = {};
    } else
        _errors.clear();
    return result;
}

Model3D Renderer3D::load_model_shared(const std::filesystem::path& path, const MeshOptions3D& options,
                                      std::string* error, Assets3D* assets) {
    std::string why;
    Model3D model;
    if (!_backend) {
        why = "3D is not available";
    } else {
        Loader::TextureResolver resolver;
        if (assets)
            resolver = [assets](const auto& source, u32 embedded, bool nearest, bool srgb, const auto& load) {
                return assets->share_texture(source, embedded, nearest, srgb, load);
            };
        model = Loader{*this, path, options, std::move(resolver)}.load(why);
    }
    if (!model.valid()) {
        KIN_LOG_WARN_F("render", "3D: model not loaded",
                       (LogFields{{.name = "path", .value = path.string()}, {.name = "reason", .value = why}}));
        // Failed assets cannot retain drawable fragments through the direct loader either.
        model = {};
    }
    if (error) {
        *error = why;
    }
    return model;
}

Status3D Scene3D::draw(const Model3D& model, const Mat4f& transform, Color tint, u32 id) {
    const bool accepted = draw_operation(model, transform, tint, id);
    return status3d(accepted, _errors);
}

bool Scene3D::draw_operation(const Model3D& model, const Mat4f& transform, Color tint, u32 id) {
    if (!_errors.empty())
        return false;
    draw_operation(model, transform, tint, id, {});
    return _errors.empty();
}
Status3D Scene3D::draw(const Model3D& model, const Mat4f& transform, Color tint, u32 id,
                       std::span<const Material3D> materials) {
    const bool accepted = draw_operation(model, transform, tint, id, materials);
    return status3d(accepted, _errors);
}

bool Scene3D::draw_operation(const Model3D& model, const Mat4f& transform, Color tint, u32 id,
                             std::span<const Material3D> materials) {
    if (!_errors.empty())
        return false;
    if (!materials.empty() && materials.size() != model.parts.size())
        return fail_submission("material count must match model parts");
    if (model.parts.size() > std::numeric_limits<u32>::max() ||
        _instances.size() > std::numeric_limits<u32>::max() - model.parts.size())
        return fail_submission("model submission exceeds the draw index range");
    for (size_t i = 0; i < model.parts.size(); ++i) {
        const auto& part = model.parts[i];
        draw_operation(part.mesh, transform * part.transform, materials.empty() ? part.material : materials[i], tint,
                       id);
    }
    return _errors.empty();
}

ModelFile3D::ModelFile3D(Renderer3D& renderer, std::filesystem::path path, const MeshOptions3D& options)
    : _renderer(&renderer), _path(std::move(path)), _options(options) {}

bool ModelFile3D::load() {
    auto loaded = _renderer->load_model(_path, _options);
    if (!loaded) {
        _errors = std::move(loaded.diagnostics);
        return false;
    }
    _model = std::move(*loaded);
    _errors.clear();
    return true;
}

Result3D<bool> ModelFile3D::poll() {
    std::error_code ec;
    const auto time = std::filesystem::last_write_time(_path, ec);
    const auto size = content_file_size(_path);
    const auto pack = content_pack_for(_path);
    const bool same_pack = !_source_pack.owner_before(pack) && !pack.owner_before(_source_pack);
    if (_attempted && time == _time && size == _source_size && same_pack) {
        if (!_errors.empty())
            return {std::nullopt, _errors}; // cached refusal; retry only after the source changes
        return result3d(false);
    }
    _attempted = true;
    _time = time;
    _source_size = size;
    _source_pack = pack;
    if (ec && !size) {
        fail3d(_errors, "ModelFile3D::poll(" + _path.string() + ")", ec.message());
        return {std::nullopt, _errors};
    }
    if (!load())
        return {std::nullopt, _errors};
    return result3d(true);
}

} // namespace kin
