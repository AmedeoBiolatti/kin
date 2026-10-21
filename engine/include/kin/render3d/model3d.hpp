#pragma once
#include <kin/render3d/errors3d.hpp>

// Models from glTF 2.0 files (.glb, .gltf) for kin's experimental 3D
// (docs/3d_plan.md): meshes, their materials (base colour and its texture,
// emissive light, alpha blending, double-sided, unlit) and where the file's
// nodes put them.
//
//     kin::Model3D gun = r3d.load_model("models/gun.glb").value_or({});
//     scene.draw(gun, transform);
//
//     kin::ModelFile3D crate{r3d, "models/crate.glb"};   // reloaded when saved
//     crate.poll();                                     // each frame
//     scene.draw(crate.model(), transform);
//
// A skinned model (a character): its skeletons and clips come with it, and a pose is turned into joint matrices
// (kin/render3d/skeleton3d.hpp, animation3d.hpp):
//
//     kin::Model3D hero = r3d.load_model("models/hero.glb").value_or({});
//     kin::AnimationPlayer3D player{&hero.skeletons[0]};
//     player.play(hero.find_clip("Walk"));
//     player.update(dt);
//     player.joint_matrices(matrices);
//     scene.draw_skinned(hero, matrices, transform);
//
// A model whose animations move plain nodes (a door, a fan, a crane's arm): those nodes are a skeleton of the model
// too, its node skeleton, posed as a skin's is and drawn rigidly (each part follows the node it hangs from):
//
//     kin::Model3D door = r3d.load_model("models/door.glb").value_or({});
//     kin::AnimationPlayer3D player{&door.skeletons[door.node_skeleton]};
//     player.play(door.find_clip("Open", door.node_skeleton), false);
//     player.update(dt);
//     player.joint_matrices(matrices);
//     scene.draw(door, transform, matrices);

#include <filesystem>
#include <kin/core/math3d.hpp>
#include <kin/render3d/animation3d.hpp>
#include <kin/render3d/mesh3d.hpp>
#include <kin/render3d/scene3d.hpp>
#include <kin/render3d/skeleton3d.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kin {

class Renderer3D;

enum class ModelPivot3D { Origin, Center, BottomCenter };
struct ModelPlacement3D {
    Vec3f position{};
    Quatf rotation{};
    Vec3f scale{1, 1, 1};
    ModelPivot3D pivot = ModelPivot3D::Origin;
    // Positive: fit the longest model-bounds dimension to this size before
    // applying scale. Zero: keep authored units. Negative/non-finite is invalid.
    f32 size = 0;
};

struct ModelPart3D {
    std::string name; // the glTF node's, or its mesh's
    Mesh3D mesh;
    Material3D material;
    // Within the model. For a skinned part (skin >= 0) it is the identity: glTF ignores the placing of a skinned mesh's
    // node, and the joint matrices place its vertices.
    Mat4f transform{};
    // The skeleton (Model3D::skeletons) whose joints move this part, or -1 for a part that is not skinned. A mesh on a
    // node under a joint, with no skin of its own, is bound to that joint (all its weight) and has the skin of the
    // joint's.
    i32 skin = -1;
    // The joint of the node skeleton (Model3D::node_skeleton) the part hangs from: its own node, or the nearest node
    // above it, that an animation moves or that carries one that is moved. -1 for a part under no animated node, and
    // for a skinned part. `transform` stays the part's place at rest; in a pose the part is at that joint's matrix
    // times `node_offset` (Model3D::part_transform).
    i32 node = -1;
    // The part's placing below that joint's node: the local placings of the nodes under it down to the part's own (the
    // identity for a mesh on the animated node itself). Made from the file's nodes, so a node scaled to zero at rest is
    // fine.
    Mat4f node_offset{};
};

// What a glTF file holds apart from what needs the GPU or the renderer: the triangles of each primitive, its skeleton
// and clips. read_model_data makes it for tests and tools; load_model makes a Model3D from the same reading.
struct ModelPartData3D {
    std::string name;
    MeshData3D mesh;
    Mat4f transform{};
    i32 skin = -1;
    i32 node = -1;       // as ModelPart3D::node
    Mat4f node_offset{}; // as ModelPart3D::node_offset
};
struct ModelData3D {
    std::vector<ModelPartData3D> parts;
    std::vector<Skeleton3D> skeletons;
    std::vector<AnimClip3D> clips;
    i32 node_skeleton = -1; // as Model3D::node_skeleton
    // Parsing can retain unsupported content for inspection. Asset loaders reject
    // this whole model when nonempty; tools must check before calling it drawable.
    std::vector<std::string> asset_diagnostics;
};

class Model3D {
public:
    std::vector<ModelPart3D> parts;
    Bounds3D bounds{}; // round all of it, in the model's space (skinned and animated parts at rest)
    // glTF skins as skeletons, and glTF animations as clips (an animation of two skins is two clips of the same name).
    // Asset loading requires a single distinct skin palette. Identical joint lists/inverse binds share one palette;
    // different palettes, oversized skins and mixed skin/plain-node animation are rejected with diagnostics.
    // Animator3D and draw_skinned(Model3D) pose skeletons[0]. A plain node between two of a skin's joints is a joint
    // of its skeleton
    // too (no vertex is bound to it), so a skeleton can have more joints than the file's skin lists. The node skeleton
    // (node_skeleton), when there is one, comes after the skins, and its clips after theirs.
    std::vector<Skeleton3D> skeletons;
    std::vector<AnimClip3D> clips;

    // The first clip called `name` for skeleton `skeleton`, or null.
    const AnimClip3D* find_clip(std::string_view name, u32 skeleton = 0) const;

    // Where part `part` is in the model for a pose of the node skeleton: `node_matrices` are that skeleton's world
    // matrices in the model's space, one a joint (its inverse binds are identities, so compute_joint_matrices,
    // compute_world_matrices and AnimationPlayer3D::joint_matrices all make them). A part that hangs from no node, or
    // from one past the matrices given, is at its `transform`; so is a `part` past the parts (the identity).
    Mat4f part_transform(std::size_t part, std::span<const Mat4f> node_matrices) const;

    // Place the chosen model-space pivot at position, retaining all glTF part
    // transforms. A positive size requires finite, non-degenerate bounds.
    // A failed result reports invalid size/bounds or non-finite transforms.
    Result3D<Mat4f> placement(const ModelPlacement3D& options = {}) const;

    bool valid() const { return !parts.empty() && asset_diagnostics.empty(); }
    explicit operator bool() const { return valid(); }

    // Loader provenance for explicit reload/diagnostics; no source bytes or
    // CPU geometry retained. Empty for programmatically constructed models.
    std::vector<std::filesystem::path> source_files;
    std::vector<std::string> asset_diagnostics;

    // The skeleton (index into skeletons) of the plain nodes the file's animations move, or -1 for none. Its joints are
    // those nodes and every node above them (parents first; names and rest poses are the nodes', inverse binds
    // identities), and the clips of an animation that moves them have it as AnimClip3D::skeleton, under the same name
    // as the animation's skin clips. Made only when a part that is not skinned hangs from one of them
    // (ModelPart3D::node), and of at most max_skin_joints nodes (more: logged, not animated). Pose3D it as any skeleton
    // and draw with Scene3D::draw(model, transform, node_matrices); a model with skins too keeps skeletons[0] for its
    // first skin.
    i32 node_skeleton = -1;
};

struct ModelBatchOptions3D {
    // World-space cell size. Zero: per-copy culling, no spatial groups.
    // Positive: group by the model transforms' world origins. Negative/non-finite is invalid.
    f32 cell_size = 16;
    // Optional replacement for all parts' materials; per-copy tints/IDs remain.
    std::optional<Material3D> material;
};

// Static copies of an entire model. Part transforms/materials and spatial
// grouping are expanded once; scene.draw_instanced(batch) retains only handles
// each frame. Cheap copies share the InstanceBatch3D records. The source Model3D
// and input span need not outlive construction. Skinned models are rejected:
// animated characters use draw_skinned or the ECS Animator3D bridge.
class ModelBatch3D {
public:
    struct Part {
        InstanceBatch3D instances;
        Material3D material;
    };
    ModelBatch3D() = default;
    static Result3D<ModelBatch3D> create(const Model3D& model, std::span<const MeshInstance3D> instances,
                                         const ModelBatchOptions3D& options = {});
    explicit operator bool() const { return !_parts.empty(); }
    std::span<const Part> parts() const { return _parts; }
    u32 instance_count() const { return _instances; }
    u64 triangle_count() const { return _triangles; }
    u64 record_bytes() const { return _record_bytes; }
    const Diagnostics3D& diagnostics() const { return _errors; }

private:
    ModelBatch3D(const Model3D& model, std::span<const MeshInstance3D> instances,
                 const ModelBatchOptions3D& options = {});
    Diagnostics3D _errors;
    std::vector<Part> _parts;
    u32 _instances = 0;
    u64 _triangles = 0, _record_bytes = 0;
};

// Reads the geometry, skins and animations of a glTF (.gltf with embedded buffers, or .glb) held in `bytes`; the files
// its buffers name are looked for beside `path` through kin's content folder or pack. No GPU, no materials or textures.
// A failed result explains why it could not read. A successful parse can retain unsupported content for tools:
// check result->asset_diagnostics before using it as drawable content. Asset loaders reject these diagnostics as a
// whole.
Result3D<ModelData3D> read_model_data(std::string_view bytes, const std::filesystem::path& path);

// A model watcher. Construction does no I/O; poll() loads initially and when the file changes.
class ModelFile3D {
public:
    // `options`: the levels of detail each mesh is made with (Renderer3D::create_mesh).
    ModelFile3D(Renderer3D& renderer, std::filesystem::path path, const MeshOptions3D& options = {});

    const Model3D& model() const { return _model; }
    // Loads it again when the file changed. A successful true payload means a
    // new version is in use; false means unchanged. Failure retains the last good
    // model and returns diagnostics. Rebind players and references into the model
    // (skeletons/clips) after a true payload, before using those references again.
    Result3D<bool> poll();
    const Diagnostics3D& diagnostics() const { return _errors; }

private:
    bool load();

    Renderer3D* _renderer = nullptr;
    std::filesystem::path _path;
    MeshOptions3D _options;
    std::filesystem::file_time_type _time{};
    Model3D _model;
    Diagnostics3D _errors;
};

} // namespace kin
