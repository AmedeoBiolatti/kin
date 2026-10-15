#pragma once
// YRD1 contains baked floats, not independently regenerated engine geometry.
#include <kin/render3d/mesh.hpp>
#include <kin/render3d/scene3d.hpp>
#include <array>
#include <bit>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace yard {
using namespace kin;
constexpr u32 route_count=1440, pose_count=120, joints=6;
struct Object {u32 mesh=0, material=0, moving=0; Mat4f transform;};
struct Material {Color colour;u32 mode=0;f32 roughness=1,metallic=0;};
struct TextureData {u32 side=0;std::vector<u8> rgba;};
struct Light {Vec3f position,target,colour;f32 radius=1,angle=45;u32 spot=0;};
struct Camera {Vec3f eye,target;};
struct Recipe {
    u32 tier=0, prop_count=0, material_count=0, actor_count=0, smoke_count=0, world_count=0, point_count=0, spot_count=0, moving_count=0;
    u64 bytes=0, mesh_bytes=0, source_triangles=0;
    std::vector<MeshData> meshes;
    std::array<u32,10> triangles{};
    std::vector<Material> materials;
    std::array<TextureData,5> textures;
    std::vector<f32> sky;
    std::array<Camera,route_count> route;
    std::vector<Object> world,props,actors,smoke;
    std::array<Vec3f,joints> pivots;
    std::vector<Mat4f> palettes,poses,motion;
    std::vector<Light> lights;
    void load(const std::string& path) {
        bytes=std::filesystem::file_size(path);
        if (bytes>16*1048576 || bytes<64) throw std::runtime_error("yard recipe exceeds 16 MiB budget or is truncated");
        std::ifstream in(path,std::ios::binary);
        const auto u=[&]() {u32 n=0;for(int i=0;i<4;++i){int c=in.get();if(c==EOF)throw std::runtime_error("truncated yard");n|=u32(c)<<(8*i);}return n;};
        const auto f=[&]() {f32 n=std::bit_cast<f32>(u());if(!std::isfinite(n)||std::abs(n)>10000)throw std::runtime_error("invalid yard float");return n;};
        const auto vec=[&]() {return Vec3f{f(),f(),f()};};
        const auto matrix=[&]() {Mat4f m;for(auto& n:m.m)n=f();if(m.m[3]!=0||m.m[7]!=0||m.m[11]!=0||m.m[15]!=1)throw std::runtime_error("invalid yard affine matrix");return m;};
        if(u()!=0x31445259||u()!=1)throw std::runtime_error("invalid yard header");
        tier=u();prop_count=u();material_count=u();actor_count=u();smoke_count=u();world_count=u();const auto mesh_count=u();
        if(u()!=route_count||u()!=pose_count||u()!=joints)throw std::runtime_error("invalid yard animation dimensions");
        point_count=u();spot_count=u();moving_count=u();if(u()!=1)throw std::runtime_error("invalid yard seed");
        if(tier>2||prop_count>20000||material_count<8||material_count>128||actor_count>256||smoke_count>2000||world_count>512||mesh_count!=10||point_count>16||spot_count>8||moving_count>prop_count)
            throw std::runtime_error("yard count limit exceeded");
        meshes.resize(mesh_count);
        for(u32 k=0;k<mesh_count;++k){
            auto& m=meshes[k];const auto nv=u(),ni=u(),nj=u();
            if(!nv||nv>10000||!ni||ni>30000||ni%3||(nj!=0&&nj!=joints))throw std::runtime_error("invalid yard mesh counts");
            mesh_bytes+=u64(nv)*sizeof(MeshVertex)+u64(ni)*4+(nj?u64(nv)*sizeof(MeshSkinVertex):0);
            if(mesh_bytes>8*1048576)throw std::runtime_error("yard mesh budget exceeded");
            m.vertices.resize(nv);m.indices.resize(ni);
            for(auto& v:m.vertices)v.position=vec();for(auto& v:m.vertices)v.normal=vec();for(auto& v:m.vertices)v.uv={f(),f()};
            for(u32 i=0;i<nv*4;++i)(void)f(); // Godot uses the same baked tangent basis.
            for(auto& i:m.indices){i=u();if(i>=nv)throw std::runtime_error("yard index out of bounds");}
            if(nj){m.skin.resize(nv);for(auto& s:m.skin)for(auto& j:s.joints){auto n=u();if(n>=joints)throw std::runtime_error("yard skin index out of bounds");j=u8(n);}for(auto& s:m.skin)for(auto& w:s.weights)w=f();}
            std::string error;if(!m.validate(&error))throw std::runtime_error(error);
            triangles[k]=ni/3;source_triangles+=ni/3;
        }
        materials.resize(material_count+2);
        for(auto& m:materials){auto c=u();m.colour={u8(c),u8(c>>8),u8(c>>16),u8(c>>24)};m.mode=u();m.roughness=f();m.metallic=f();if(m.mode>2||m.roughness<0||m.roughness>1||m.metallic<0||m.metallic>1)throw std::runtime_error("invalid yard material");}
        for(auto& t:textures){t.side=u();if(t.side!=64&&t.side!=128)throw std::runtime_error("invalid yard texture");t.rgba.resize(t.side*t.side*4);in.read(reinterpret_cast<char*>(t.rgba.data()),t.rgba.size());if(!in)throw std::runtime_error("truncated yard texture");}
        if(u()!=128||u()!=64)throw std::runtime_error("invalid yard sky dimensions");sky.resize(128*64*3);for(auto& v:sky)v=f();
        for(auto& c:route){c.eye=vec();c.target=vec();if(length_squared(c.eye-c.target)<.001f)throw std::runtime_error("invalid yard camera");}
        const auto objects=[&](std::vector<Object>& out,u32 count){out.resize(count);for(auto& o:out){o.mesh=u();o.material=u();o.moving=u();o.transform=matrix();if(o.mesh>=mesh_count||o.material>=materials.size()||o.moving>1)throw std::runtime_error("invalid yard object");}};
        objects(world,world_count);objects(props,prop_count);objects(actors,actor_count);objects(smoke,smoke_count);
        u32 moving=0;for(const auto& o:props)moving+=o.moving;if(moving!=moving_count)throw std::runtime_error("yard moving count mismatch");
        for(auto& p:pivots)p=vec();palettes.resize(pose_count*joints);poses.resize(pose_count*joints);
        for(u32 p=0;p<pose_count;++p)for(u32 j=0;j<joints;++j){palettes[p*joints+j]=matrix();poses[p*joints+j]=matrix();}
        motion.resize(pose_count);for(auto& m:motion)m=matrix();
        lights.resize(point_count+spot_count);for(auto& l:lights){l.position=vec();l.target=vec();l.colour=vec();l.radius=f();l.angle=f();l.spot=u();if(l.radius<=0||l.radius>100||l.spot>1||l.angle<=0||l.angle>=90)throw std::runtime_error("invalid yard light");}
        if(in.peek()!=EOF)throw std::runtime_error("trailing yard data");
    }
    Mat4f moved(const Object& o,u32 frame,u32 id) const {
        const Mat4f& delta=motion[(frame+id*17)%pose_count];auto out=delta*o.transform;
        for(u32 k=12;k<15;++k)out.m[k]=o.transform.m[k]+delta.m[k];return out;
    }
};
} // namespace yard
