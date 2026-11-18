#pragma once
// Baked lighting for a world's entities (docs/rendering_3d.md, "Baked lighting on entities"): what stands still is
// marked StaticLighting3D, lights choose their part (PointLight3D::bake, SpotLight3D::bake, the active
// SceneEnvironment3D's sun_bake and fill_bake), bake_scene_lighting bakes them into the content, and the environment's
// `baked_lighting` names the bake that SceneBakedLighting3D puts in the scene each frame (SceneSession3D does it; a
// game that renders scene() itself runs one).
#include <filesystem>
#include <kin/ecs/world.hpp>
#include <kin/render3d/baked_lighting3d.hpp>
#include <kin/render3d/errors3d.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <string>

namespace kin {

class SceneBakedLighting3D {
public:
    // `content`: where the environment's `baked_lighting` folder starts (empty: as given).
    explicit SceneBakedLighting3D(Renderer3D& renderer, std::filesystem::path content = {});

    // Sets `scene.baked_lighting` to the bake the active SceneEnvironment3D names (the enabled one of the highest
    // priority, as apply_scene3d_settings chooses), loaded through the content folder or pack the first time it is
    // named; empty when it names none. A bake that cannot be loaded is an error once, and is not tried again until
    // the name changes or reload().
    Status3D update(EcsWorld& world, Scene3D& scene);
    // Loads it again at the next update (after a bake into the same folder).
    void reload();
    const BakedLighting3D& baked() const { return _baked; }

private:
    Renderer3D& _renderer;
    std::filesystem::path _content;
    std::string _name;   // the folder loaded (or that failed)
    bool _tried = false; // _name was tried
    BakedLighting3D _baked;
};

// Bakes `world` as it stands (its WorldTransform3D: after a frame was collected): every enabled StaticLighting3D
// entity's MeshRenderer3D, or ModelRenderer3D's parts that neither bend nor morph (their meshes as drawn, read back
// from the GPU), with a lightmap where it is `lightmapped` and its mesh has lightmap UVs; the light of every
// PointLight3D and SpotLight3D and of the active SceneEnvironment3D's sun, fill and ambient light (each by its
// LightBake3D); made as that environment's `bake` says. Writes it to `content` / `folder` (save_baked_lighting), and
// sets each entity's StaticLighting3D::lightmap and the environment's `baked_lighting` to `folder` (save the scene to
// keep them). The sky's light is the scene's as drawn: the procedural sky's panorama (Lights3D::sky_lights_scene, as
// the last render made it), else the environment's picture (Lights3D::environment), else the ambient and ground
// ambient light; of `scene` when given (a game's own environment picture), else the active environment's. Materials
// take part by their colour, metalness and emission, with their base colour and emission maps (read
// back from the GPU at up to 256 texels a side).
Result3D<LightBakeStats3D> bake_scene_lighting(EcsWorld& world, Renderer3D& renderer,
                                               const std::filesystem::path& content,
                                               const std::filesystem::path& folder = "lighting",
                                               const LightBakeControl3D& control = {}, const Scene3D* scene = nullptr);

} // namespace kin
