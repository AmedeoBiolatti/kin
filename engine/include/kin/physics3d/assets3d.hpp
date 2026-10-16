#pragma once
#include <kin/assets/asset_handle.hpp>
#include <kin/physics3d/shape3d.hpp>
#include <kin/render3d/model_geometry3d.hpp>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>
namespace kin {
struct ColliderRecipe3D {
    Shape3DKind kind=Shape3DKind::Box;
    Vec3f half_extents{.5f,.5f,.5f},offset{},scale{1,1,1};
    Quatf rotation{};
    f32 radius=.5f,half_height=.5f;
    std::filesystem::path mesh;
    std::vector<Vec3f> points;
    std::vector<u32> indices;
    std::vector<ColliderRecipe3D> children;
};
struct ColliderAssetLimits3D {
    u64 source_bytes=1024u*1024u;
    ModelGeometryLimits3D geometry;
    std::size_t nodes=64,depth=4,hull_points=256,cache_entries=128;
};
struct ColliderRecipeLoad3D {
    std::optional<ColliderRecipe3D> recipe;
    std::vector<std::string> diagnostics;
    bool ok() const { return recipe.has_value() && diagnostics.empty(); }
};
ColliderRecipeLoad3D parse_collider3d_asset(std::string_view text,ColliderAssetLimits3D limits={});
struct CookedCollider3D {
    Shape3D shape;
    Vec3f scale{1,1,1};
    bool contains_surface=false; // triangle meshes cannot be dynamic
    std::vector<std::filesystem::path> source_files;
};
// CPU-only, synchronous. Holds bounded weak lookup metadata, never geometry or
// owning shape caches. Keep handles in the scene. Root-relative paths and packs
// follow the same content utilities as rendering assets. Explicit invalidation.
class PhysicsAssets3D {
public:
    struct Stats { std::size_t entries=0;u64 cooks=0,hits=0; };
    explicit PhysicsAssets3D(std::filesystem::path root=".");
    PhysicsAssets3D(std::filesystem::path root,ColliderAssetLimits3D limits);
    ~PhysicsAssets3D();
    PhysicsAssets3D(const PhysicsAssets3D&)=delete;
    PhysicsAssets3D& operator=(const PhysicsAssets3D&)=delete;
    AssetHandle<CookedCollider3D> load_collider(const std::filesystem::path& path,Vec3f scale={1,1,1},std::string* error=nullptr);
    void invalidate(const std::filesystem::path& source);
    void trim();void clear();
    Stats stats() const;
    const std::filesystem::path& root() const;
private:
    struct Data;std::unique_ptr<Data> _data;
};
}
