#pragma once

// Models from glTF 2.0 files (.glb, .gltf) for kin's experimental 3D
// (docs/3d_plan.md): meshes, their materials (base colour and its texture,
// emissive light, alpha blending, double-sided, unlit) and where the file's
// nodes put them.
//
//     kin::Model gun = r3d.load_model("models/gun.glb");
//     scene.draw(gun, transform);
//
//     kin::ModelFile crate{r3d, "models/crate.glb"};   // reloaded when saved
//     crate.poll();                                     // each frame
//     scene.draw(crate.model(), transform);

#include <kin/core/math3d.hpp>
#include <kin/render3d/mesh.hpp>
#include <kin/render3d/scene3d.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace kin {

class Renderer3D;

struct ModelPart {
    std::string name; // the glTF node's, or its mesh's
    Mesh mesh;
    Material3D material;
    Mat4f transform{}; // within the model
};

class Model {
public:
    std::vector<ModelPart> parts;
    Bounds3 bounds{}; // round all of it, in the model's space

    bool valid() const { return !parts.empty(); }
    explicit operator bool() const { return valid(); }
};

// A model kept loaded from its file, loaded again when the file changes.
class ModelFile {
public:
    ModelFile(Renderer3D& renderer, std::filesystem::path path);

    const Model& model() const { return _model; }
    // Loads it again when the file changed since the last look; true when a
    // new version is in use (one that fails to load is logged, the last kept).
    bool poll();
    const std::string& error() const { return _error; }

private:
    bool load();

    Renderer3D* _renderer = nullptr;
    std::filesystem::path _path;
    std::filesystem::file_time_type _time{};
    Model _model;
    std::string _error;
};

} // namespace kin
