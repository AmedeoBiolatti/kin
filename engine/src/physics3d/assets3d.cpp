#include <kin/physics3d/assets3d.hpp>
#include <kin/ecs/physics3d.hpp>
#include <kin/ecs/component.hpp>
#include <kin/assets/content.hpp>
#include <kin/core/json_value.hpp>
#include <algorithm>
#include <cmath>
#include <list>
#include <stdexcept>

namespace kin {
namespace {
void require(bool value,const std::string& message) { if(!value) throw std::runtime_error(message); }
bool finite(Vec3f v) { return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z); }
bool positive(Vec3f v) { return finite(v)&&v.x>0&&v.y>0&&v.z>0; }
void fields(const JsonValue& v,std::initializer_list<std::string_view> allowed) {
    require(v.is_object(),"expected collider object");
    for(const auto& [name,ignored]:v.members()) { (void)ignored;require(std::find(allowed.begin(),allowed.end(),name)!=allowed.end(),"unknown collider field '"+name+"'"); }
}
template<class T> T value(const JsonValue& v,ComponentFieldKind kind) {
    std::string error;auto field=component_field_value_from_json(kind,v,error);
    require(bool(field),error);return std::get<T>(*field);
}
ColliderRecipe3D parse(const JsonValue& json,ColliderAssetLimits3D limits,size_t depth,size_t& nodes,size_t& vertices,size_t& triangles) {
    require(depth<limits.depth && ++nodes<=limits.nodes,"collider tree exceeds depth/node budget");
    fields(json,{"kind","half_extents","radius","half_height","offset","rotation","scale","mesh","points","indices","children"});
    const auto* kind=json.find("kind");require(kind&&kind->is_string(),"collider requires kind");
    ColliderRecipe3D result;
    const auto& name=kind->as_string();
    if(name=="box") result.kind=Shape3DKind::Box;
    else if(name=="sphere") result.kind=Shape3DKind::Sphere;
    else if(name=="capsule") result.kind=Shape3DKind::Capsule;
    else if(name=="cylinder") result.kind=Shape3DKind::Cylinder;
    else if(name=="convex_hull") result.kind=Shape3DKind::ConvexHull;
    else if(name=="triangle_mesh") result.kind=Shape3DKind::TriangleMesh;
    else if(name=="compound") result.kind=Shape3DKind::Compound;
    else throw std::runtime_error("unknown collider kind '"+name+"'");
#define VECTOR(field) if(const auto* v=json.find(#field)) result.field=value<Vec3f>(*v,ComponentFieldKind::Vec3f)
    VECTOR(half_extents);VECTOR(offset);VECTOR(scale);
#undef VECTOR
    if(const auto* v=json.find("rotation")) result.rotation=value<Quatf>(*v,ComponentFieldKind::Quatf);
#define NUMBER(field) if(const auto* v=json.find(#field)) { require(v->is_number()&&std::isfinite(v->as_number())&&v->as_number()>0&&v->as_number()<=1e6,"invalid " #field);result.field=f32(v->as_number()); }
    NUMBER(radius);NUMBER(half_height);
#undef NUMBER
    require(positive(result.scale)&&positive(result.half_extents),"collider scale/dimensions must be finite and positive");
    if(const auto* v=json.find("mesh")) {
        require(v->is_string()&&!v->as_string().empty()&&v->as_string().size()<=4096,"mesh requires a content path");result.mesh=v->as_string();
    }
    if(const auto* v=json.find("points")) {
        require(v->is_array()&&v->items().size()<=limits.geometry.vertices-vertices,"point budget exceeded");
        for(const auto& point:v->items()) result.points.push_back(value<Vec3f>(point,ComponentFieldKind::Vec3f));
        vertices+=result.points.size();
    }
    if(const auto* v=json.find("indices")) {
        require(v->is_array()&&v->items().size()%3==0&&v->items().size()/3<=limits.geometry.triangles-triangles,"triangle budget exceeded");
        for(const auto& index:v->items()) {
            require(index.is_number()&&std::isfinite(index.as_number())&&index.as_number()>=0&&index.as_number()<result.points.size()&&std::floor(index.as_number())==index.as_number(),"index outside point array");
            result.indices.push_back(u32(index.as_number()));
        }
        triangles+=result.indices.size()/3;
    }
    if(const auto* v=json.find("children")) {
        require(v->is_array()&&v->items().size()<=limits.nodes-nodes,"child budget exceeded");
        for(const auto& child:v->items()) result.children.push_back(parse(child,limits,depth+1,nodes,vertices,triangles));
    }
    const bool mesh=result.kind==Shape3DKind::ConvexHull||result.kind==Shape3DKind::TriangleMesh;
    require(mesh || (result.mesh.empty()&&result.points.empty()&&result.indices.empty()),"geometry only allowed for hull/triangle mesh");
    require((result.kind==Shape3DKind::Compound)==!result.children.empty(),"only compounds have children; a compound must have children");
    if(mesh) {
        require(result.mesh.empty()!=result.points.empty(),"choose exactly one mesh asset or point array");
        if(result.kind==Shape3DKind::ConvexHull) require(result.indices.empty(),"a convex hull uses points, not indices");
        if(result.kind==Shape3DKind::TriangleMesh&&result.mesh.empty()) require(!result.indices.empty(),"triangle meshes require indices");
    }
    return result;
}
bool uniform(Vec3f v) { return std::abs(v.x-v.y)<1e-5f&&std::abs(v.y-v.z)<1e-5f; }
bool volumetric(std::span<const Vec3f> points) {
    // Jolt may give coplanar input a small artificial thickness. Require a real
    // volume for authored hulls; surface collision is an explicit mesh recipe.
    const auto origin=points.front();Vec3f edge{},normal{};f32 edge2=0,area2=0;
    for(auto p:points) {const auto v=p-origin;const auto n=dot(v,v);if(n>edge2){edge=v;edge2=n;}}
    for(auto p:points) {const auto v=cross(edge,p-origin);const auto n=dot(v,v);if(n>area2){normal=v;area2=n;}}
    if(!std::isfinite(edge2)||!std::isfinite(area2)||edge2<=0||area2<=0)return false;
    f32 distance=0;for(auto p:points)distance=std::max(distance,std::abs(dot(normal,p-origin)));
    return std::isfinite(distance)&&distance/std::sqrt(area2)>std::sqrt(edge2)*1e-5f;
}
struct Cook {
    PhysicsAssets3D& assets;ColliderAssetLimits3D limits;size_t vertices=0,triangles=0;
    CookedCollider3D output;
    Collider3D collider(const ColliderRecipe3D& recipe,Vec3f scale) {
        require(uniform(scale)||recipe.rotation==Quatf{},"rotated collider children require uniform parent scale");
        Collider3D result;result.half_extents=recipe.half_extents;result.radius=recipe.radius;result.half_height=recipe.half_height;
        result.offset=recipe.offset;result.rotation=recipe.rotation;result.scale=recipe.scale;
        switch(recipe.kind) {
        case Shape3DKind::Box:result.kind=Collider3D::Kind::Box;break;
        case Shape3DKind::Sphere:result.kind=Collider3D::Kind::Sphere;break;
        case Shape3DKind::Capsule:result.kind=Collider3D::Kind::Capsule;break;
        case Shape3DKind::Cylinder:result.kind=Collider3D::Kind::Cylinder;break;
        case Shape3DKind::Compound:result.kind=Collider3D::Kind::Compound;break;
        case Shape3DKind::ConvexHull:result.kind=Collider3D::Kind::ConvexHull;break;
        case Shape3DKind::TriangleMesh:result.kind=Collider3D::Kind::TriangleMesh;output.contains_surface=true;break;
        default:throw std::runtime_error("unsupported collision shape");
        }
        for(const auto& child:recipe.children) result.children.push_back(collider(child,scale*recipe.scale));
        if(result.kind==Collider3D::Kind::ConvexHull||result.kind==Collider3D::Kind::TriangleMesh) {
            ModelGeometry3D geometry;
            if(!recipe.mesh.empty()) {
                auto budget=limits.geometry;budget.vertices-=vertices;budget.triangles-=triangles;
                std::string error;
                const bool loaded=read_model_geometry3d(assets.root()/recipe.mesh,geometry,budget,&error);
                require(loaded,error);
                output.source_files.insert(output.source_files.end(),geometry.source_files.begin(),geometry.source_files.end());
            } else { geometry.vertices=recipe.points;geometry.indices=recipe.indices; }
            require(geometry.vertices.size()<=limits.geometry.vertices-vertices && geometry.indices.size()/3<=limits.geometry.triangles-triangles,"aggregate collider geometry budget exceeded");
            vertices+=geometry.vertices.size();triangles+=geometry.indices.size()/3;
            if(result.kind==Collider3D::Kind::ConvexHull) {
                std::ranges::sort(geometry.vertices,[](Vec3f a,Vec3f b) { return a.x!=b.x ? a.x<b.x : a.y!=b.y ? a.y<b.y : a.z<b.z; });
                geometry.vertices.erase(std::unique(geometry.vertices.begin(),geometry.vertices.end()),geometry.vertices.end());
                require(geometry.vertices.size()>=4&&geometry.vertices.size()<=limits.hull_points,"convex hull point budget exceeded (or fewer than four points)");
                require(volumetric(geometry.vertices),"convex hull points must enclose volume");
            }
            auto mesh=std::make_shared<MeshData3D>();mesh->vertices.resize(geometry.vertices.size());
            for(size_t i=0;i<geometry.vertices.size();++i) mesh->vertices[i].position=geometry.vertices[i];
            if(result.kind==Collider3D::Kind::TriangleMesh) mesh->indices=std::move(geometry.indices);
            result.mesh=std::move(mesh);
        }
        return result;
    }
};
}
ColliderRecipeLoad3D parse_collider3d_asset(std::string_view text,ColliderAssetLimits3D limits) {
    ColliderRecipeLoad3D result;
    try {
        require(text.size()<=limits.source_bytes,"collider source exceeds byte budget");auto json=parse_json(text);require(json.ok(),json.error);
        fields(*json.value,{"schema","shape"});
        require(json.value->string_at("schema")=="kin.collider3d/1"&&json.value->find("shape"),"invalid collider schema/shape");
        size_t nodes=0,vertices=0,triangles=0;result.recipe=parse(*json.value->find("shape"),limits,0,nodes,vertices,triangles);
    } catch(const std::exception& error) { result.diagnostics.push_back(error.what()); }
    return result;
}
struct PhysicsAssets3D::Data {
    struct Entry {std::filesystem::path path;Vec3f scale;std::weak_ptr<AssetStorage<CookedCollider3D>> storage;};
    std::filesystem::path root;ColliderAssetLimits3D limits;Stats stats;std::list<Entry> entries;
};
PhysicsAssets3D::PhysicsAssets3D(std::filesystem::path root):PhysicsAssets3D(std::move(root),{}){}
PhysicsAssets3D::PhysicsAssets3D(std::filesystem::path root,ColliderAssetLimits3D limits):_data(std::make_unique<Data>()) {
    require(limits.source_bytes&&limits.nodes&&limits.depth&&limits.depth<=4&&limits.hull_points>=4,"invalid collider limits");
    _data->root=std::filesystem::absolute(root).lexically_normal();_data->limits=limits;
}
PhysicsAssets3D::~PhysicsAssets3D()=default;
const std::filesystem::path& PhysicsAssets3D::root() const {return _data->root;}
void PhysicsAssets3D::trim(){_data->entries.remove_if([](const auto& entry){return entry.storage.expired();});}
void PhysicsAssets3D::clear(){_data->entries.clear();}
PhysicsAssets3D::Stats PhysicsAssets3D::stats() const {auto stats=_data->stats;stats.entries=_data->entries.size();return stats;}
void PhysicsAssets3D::invalidate(const std::filesystem::path& source) {
    const auto path=(_data->root/source).lexically_normal();
    _data->entries.remove_if([&](const auto& entry){auto live=entry.storage.lock();return !live||entry.path==path||std::ranges::find(live->data.source_files,path)!=live->data.source_files.end();});
}
AssetHandle<CookedCollider3D> PhysicsAssets3D::load_collider(const std::filesystem::path& source,Vec3f scale,std::string* error) {
    auto& d=*_data;const auto path=(d.root/source).lexically_normal();trim();
    for(auto it=d.entries.begin();it!=d.entries.end();++it) if(it->path==path&&it->scale==scale) if(auto live=it->storage.lock()) {
        ++d.stats.hits;d.entries.splice(d.entries.end(),d.entries,it);if(error)error->clear();return AssetHandle<CookedCollider3D>{std::move(live)};
    }
    try {
        require(positive(scale),"collision scale must be finite and positive");
        const auto size=content_file_size(path);require(size&&*size<=d.limits.source_bytes,"collider source missing or exceeds byte budget");
        const auto text=read_content_text(path);require(bool(text),"cannot read collider recipe");
        auto parsed=parse_collider3d_asset(*text,d.limits);require(parsed.ok(),parsed.diagnostics.empty()?"invalid collider":parsed.diagnostics.front());
        Cook cook{*this,d.limits};cook.output.scale=scale;cook.output.source_files.push_back(path);
        { const auto description=cook.collider(*parsed.recipe,scale);++d.stats.cooks;cook.output.shape=cook_collider3d(description,scale); }
        require(bool(cook.output.shape),"collision cooking failed (degenerate geometry or unsupported shape)");
        std::ranges::sort(cook.output.source_files);cook.output.source_files.erase(std::unique(cook.output.source_files.begin(),cook.output.source_files.end()),cook.output.source_files.end());
        auto storage=std::make_shared<AssetStorage<CookedCollider3D>>();storage->data=std::move(cook.output);storage->is_loaded=true;
        if(d.limits.cache_entries) {while(d.entries.size()>=d.limits.cache_entries)d.entries.pop_front();d.entries.push_back({path,scale,storage});}
        if(error)error->clear();return AssetHandle<CookedCollider3D>{std::move(storage)};
    } catch(const std::exception& failure) {if(error)*error=path.string()+": "+failure.what();return {};}
}
}
