#pragma once
#include <filesystem>
#include <functional>
#include <kin/assets/asset_handle.hpp>
#include <kin/render3d/errors3d.hpp>
#include <kin/render3d/material_asset3d.hpp>
#include <kin/render3d/model3d.hpp>
#include <memory>

namespace kin {

// Renderer-thread-only model loading. Keep returned handles (or shared()) in
// the scene that owns the art. The library holds WEAK references: dropping the
// last model/mesh/material handle releases resources without a cache pinning
// them. Renderer3D and its Renderer2D must outlive this library and its handles.
//
//   Assets3D art{r3d, content_root()};
//   auto tree = art.load_model("trees/pine.glb").value_or({});
//   if (tree) scene.draw(*tree, tree->placement({.position = {2, 0, 0}}).value_or(Mat4f::identity()));
//
// Paths are relative to the fixed root (absolute paths are accepted). Lexical
// aliases share a key; symlink aliases do not. Mounted content packs work as
// with load_model(). Files/pack remounts are not watched: clear() before loading
// replacements. Failed loads are never cached. GPU loading must not be passed
// to AssetServer worker threads; this uses the existing AssetHandle ownership
// contract, not an asynchronous upload service.
class Assets3D {
public:
    struct Limits {
        // Bounds on weak lookup metadata, not a limit on caller-owned assets or
        // total GPU memory. Oldest lookup entries are forgotten at capacity;
        // live handles stay valid. Zero disables that lookup cache.
        std::size_t models = 128;
        std::size_t textures = 512;
        std::size_t materials = 128;
        std::size_t shaders = 64;
    };
    struct Stats {
        std::size_t model_entries = 0, texture_entries = 0;
        u64 model_hits = 0, texture_hits = 0;
        u64 model_loads = 0, texture_loads = 0;
        std::size_t material_entries = 0;
        u64 material_hits = 0, material_loads = 0;
        std::size_t shader_entries = 0;
        u64 shader_hits = 0, shader_loads = 0;
    };

    explicit Assets3D(Renderer3D& renderer, std::filesystem::path root = ".");
    Assets3D(Renderer3D& renderer, std::filesystem::path root, Limits limits);
    ~Assets3D();
    Assets3D(const Assets3D&) = delete;
    Assets3D& operator=(const Assets3D&) = delete;

    // Without 3D (Renderer3D::available() false: a headless session, a server) a CPU-only model: its parts' names,
    // transforms and skins, each with an empty mesh handle that keeps its vertex/index counts, bounds and joint count,
    // and the model's skeletons, clips and bounds. No GPU memory; parts keep the default material, and only the model's
    // own file is a source (its images are not decoded).
    Result3D<AssetHandle<Model3D>> load_model(const std::filesystem::path& path);
    Result3D<AssetHandle<Model3D>> load_model(const std::filesystem::path& path, const MeshOptions3D& options);
    // Shares the colour/data image cache with imported glTF maps. This legacy API
    // also keys on nearest: glTF maps use full-mip images and independent samplers.
    Result3D<Texture> load_texture(const std::filesystem::path& path, bool nearest = false, bool srgb = true);
    // Checked SPIR-V made into a shader. Without 3D, a stand-in handle that nothing draws with, so that materials
    // naming a shader still bind.
    Result3D<MaterialShader3D> load_material_shader(const std::filesystem::path& path);
    // Patches imported materials without cloning any model/mesh. Empty variant
    // selects the base layer. Invalidate changed sources explicitly before rebinding.
    Result3D<AssetHandle<MaterialBinding3D>> load_materials(const std::filesystem::path& path,
                                                            const std::shared_ptr<const Model3D>& model,
                                                            std::string_view variant = {});
    // Forget expired weak entries; no live resource is unloaded.
    void trim();
    // Forget all model/texture lookups. Existing handles, batches and scenes
    // remain valid; subsequent loads read fresh versions from content.
    void clear();
    // Forget one source and models that depend on it, including their embedded
    // texture lookups. Active handles remain valid. No I/O or resource unload.
    void invalidate(const std::filesystem::path& source);
    Stats stats() const;
    const std::filesystem::path& root() const;
    // Last load's diagnostic, including cache hits (which clear it).
    const Diagnostics3D& diagnostics() const;

private:
    AssetHandle<Model3D> load_model_operation(const std::filesystem::path& path, const MeshOptions3D& options);
    AssetHandle<Model3D> load_model_operation(const std::filesystem::path& path);
    Texture load_texture_operation(const std::filesystem::path& path, bool nearest = false, bool srgb = true);
    MaterialShader3D load_material_shader_operation(const std::filesystem::path& path);
    AssetHandle<MaterialBinding3D> load_materials_operation(const std::filesystem::path& path,
                                                            const std::shared_ptr<const Model3D>& model,
                                                            std::string_view variant = {});
    friend class Renderer3D;
    Texture share_texture(const std::filesystem::path& source, u32 embedded_image, bool nearest, bool srgb,
                          const std::function<Texture()>& load);
    struct Data;
    std::unique_ptr<Data> _data;
    AssetHandle<Model3D> load_model_impl(const std::filesystem::path&, const MeshOptions3D&, std::string*);
    Texture load_texture_impl(const std::filesystem::path&, bool, bool, std::string*);
    MaterialShader3D load_material_shader_impl(const std::filesystem::path&, std::string*);
    AssetHandle<MaterialBinding3D> load_materials_impl(const std::filesystem::path&,
                                                       const std::shared_ptr<const Model3D>&, std::string_view,
                                                       std::string*);
};

} // namespace kin
