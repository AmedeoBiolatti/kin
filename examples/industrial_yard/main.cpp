// A bounded, shared yard workload: independent props, distributed edits and real skinning.
#include "recipe.hpp"
#include <kin/core/jobs.hpp>
#include <kin/core/json.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <kin/runtime/scene_app.hpp>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>

namespace {
using namespace kin;
using Clock=std::chrono::steady_clock;
constexpr int default_warmup=300;
struct Options {
    std::string recipe,metrics;int warmup=default_warmup,frames=1440,scale=1,workers=4,points=-1,actors=-1,smoke=-1,props=-1,offset=0;
    bool gpu_pass_markers=false,receives=true,validate=false,animation=true,motion=true,glass=true,structures=true,shadows=true,taa=false,ao=false,contact=false,profile=false,visible=false,preview=false,rebuild=false;
};
double cpu(bool process){
#ifdef __linux__
    timespec t{};clock_gettime(process?CLOCK_PROCESS_CPUTIME_ID:CLOCK_THREAD_CPUTIME_ID,&t);return double(t.tv_sec)*1000+double(t.tv_nsec)/1e6;
#else
    return 0;
#endif
}
class Yard final:public Scene {
public:
    Yard(Options options,JobSystem* jobs):_o(std::move(options)),_jobs(jobs){
        _csv.open(_o.metrics);if(!_csv)throw std::runtime_error("cannot write yard CSV");
        _csv<<"frame,route_frame,frame_interval_ms,scene_build_ms,cpu_submit_ms,render_main_cpu_ms,render_process_cpu_ms,draws,triangles,instances,culled,lights,spots,local_shadow_maps,local_shadow_draws,local_shadow_triangles,local_shadow_casters,skinned_copies,skin_matrices,retained_upload_bytes,instance_upload_bytes,prepared_reused,prepared_draws,feature_bytes,local_shadow_bytes,moving_x_sum,moving_y_sum,moving_z_sum,profile_mono_ms\n";
    }
    std::string_view name()const override{return "Industrial yard";}
    void update(SceneContext& ctx)override{if(_o.preview&&ctx.input.frame_pressed(Key::Escape))ctx.app.quit();}
    void render(SceneContext& ctx)override{
        if(!_r)load(ctx);
        const auto begin=Clock::now();const auto interval=std::chrono::duration<double,std::milli>(begin-_previous).count();_previous=begin;
        _route=u32(std::max(0,_frame-_o.warmup)+_o.offset)%yard::route_count;
        _sums={};
        for(auto& batch:_dynamic){
            for(u32 i=0;i<batch.indices.size();++i){auto id=batch.indices[i];if(_o.motion)batch.records[i].transform=_data.moved(_data.props[id],_route,id);for(int k=0;k<3;++k)_sums[k]+=batch.records[i].transform.m[k+12];}
            if(_o.motion)batch.batch.update(0,batch.records);
        }
        if(!_ready||(_o.rebuild&&_o.actors&&_o.animation)){
            _scene.clear();
            _actor_joints.clear();
            if(_o.structures)for(const auto& o:_data.world)if(_o.glass||o.material<_data.material_count)draw(o);
            for(int i=0;i<_o.props;++i)if(!_data.props[i].moving)draw(_data.props[i]);
            for(const auto& b:_dynamic)_scene.draw_instanced(b.batch,_materials[b.material]);
            for(int i=0;i<_o.actors;++i){const auto& o=_data.actors[i];const auto pose=((_o.animation?_route:0)+u32(i)*13)%yard::pose_count;_actor_joints.push_back(u32(_scene.joint_matrices().size()));_scene.draw_skinned(_meshes[o.mesh],{_data.palettes.data()+pose*yard::joints,yard::joints},o.transform,_materials[o.material],colors::white,30000+i);}
            for(int i=0;i<_o.smoke;++i)draw(_data.smoke[i]);
            _ready=true;
        }else if(_o.animation){
            for(int i=0;i<_o.actors;++i){const auto pose=(_route+u32(i)*13)%yard::pose_count;_scene.update_joint_matrices(_actor_joints[i],{_data.palettes.data()+pose*yard::joints,yard::joints});}
        }
        configure();const auto& c=_data.route[_route];_scene.camera=Camera3D::look_at(c.eye,c.target,70);_scene.camera.z_far=180;
        const auto submitted=Clock::now();const double main=_o.profile?cpu(false):0,process=_o.profile?cpu(true):0;
        if(!_r->render(_view,_scene))throw std::runtime_error("yard render failed");
        const auto main_ms=_o.profile?cpu(false)-main:0,process_ms=_o.profile?cpu(true)-process:0;
        const double submit_ms=std::chrono::duration<double,std::milli>(Clock::now()-submitted).count();
        ctx.renderer.clear(Color::rgb(15,22,35));ctx.renderer.draw_texture(_view.texture(),{0,0,1280,720});
        if(_frame>_o.warmup){const auto& st=_r->debug_stats();_csv<<std::fixed<<std::setprecision(6)<<_frame<<','<<_route<<','<<interval<<','<<std::chrono::duration<double,std::milli>(submitted-begin).count()<<','<<submit_ms<<','<<main_ms<<','<<process_ms<<','<<st.draws<<','<<st.triangles<<','<<st.instances<<','<<st.culled<<','<<st.lights<<','<<st.spots<<','<<st.local_shadow_maps<<','<<st.local_shadow_draws<<','<<st.local_shadow_triangles<<','<<st.local_shadow_casters<<','<<st.skinned_copies<<','<<st.skin_matrices<<','<<st.retained_upload_bytes<<','<<st.instance_upload_bytes<<','<<st.prepared_reused<<','<<st.prepared_draws<<','<<st.feature_bytes<<','<<st.local_shadow_bytes<<','<<_sums[0]<<','<<_sums[1]<<','<<_sums[2]<<','<<std::chrono::duration<double,std::milli>(begin.time_since_epoch()).count()<<'\n';}
        ++_frame;
        if(_o.visible&&!_o.preview&&_frame>_o.warmup+_o.frames)ctx.app.quit();
    }
    void write_report(JsonWriter& j)const override{
        j.field("recipe",_o.recipe).field("tier",_data.tier).field("props",_o.props).field("actors",_o.actors).field("smoke",_o.smoke).field("materials",_data.material_count)
            .field("structures",_o.structures).field("glass",_o.glass).field("motion",_o.motion).field("animation",_o.animation).field("shadows",_o.shadows).field("taa",_o.taa).field("ao",_o.ao).field("contact",_o.contact)
            .field("point_lights",_o.points).field("source_point_lights",_data.point_count).field("receive_shadows",_o.receives).field("spot_lights",_data.spot_count).field("shadow_resolution",1024).field("shadow_near",.025).field("shadow_depth_bits",32)
            .field("render_width",1280*_o.scale).field("render_height",720*_o.scale).field("route_frame",_route).field("route_offset",_o.offset).field("measured_frames",_o.frames).field("warmup_frames",_o.warmup)
            .field("moving_props",_moving).field("moving_batches",u64(_dynamic.size())).field("source_triangles",_data.source_triangles).field("scene_triangles",_triangles).field("source_mesh_bytes",_data.mesh_bytes).field("recipe_bytes",_data.bytes)
            .field("moving_x_sum",_sums[0]).field("moving_y_sum",_sums[1]).field("moving_z_sum",_sums[2]).field("renderer_workers",_o.workers).field("preview",_o.preview).field("visible",_o.visible);
        if(_r){const auto& st=_r->debug_stats();j.field("lights_used",st.lights).field("spots_used",st.spots).field("local_shadow_maps",st.local_shadow_maps).field("local_shadow_draws",st.local_shadow_draws).field("local_shadow_bytes",st.local_shadow_bytes).field("skin_matrices",st.skin_matrices).field("skinned_copies",st.skinned_copies).field("prepared_reused",st.prepared_reused).field("prepared_draws",st.prepared_draws);}
    }
private:
    void draw(const yard::Object& o){_scene.draw(_meshes[o.mesh],o.transform,_materials[o.material]);}
    void configure(){
        _scene.debug.gpu_pass_markers=_o.gpu_pass_markers;
        auto& l=_scene.lights;l.points.clear();l.spots.clear();l.sun={.direction={-.5f,1,.35f},.color={.25f,.2f,.15f}};l.ambient={.035f,.045f,.065f};l.shadow_area={};l.environment=_environment;l.local_shadow_resolution=1024;l.max_shadowed_points=0;l.max_shadowed_spots=_data.spot_count;l.cache_shadows=false;
        u32 point=0;for(u32 i=0;i<_data.lights.size();++i){const auto& d=_data.lights[i];if(d.spot){SpotLight s{.position=d.position,.direction=normalize(d.target-d.position),.color=d.colour,.radius=d.radius,.inner_angle=30,.outer_angle=d.angle};s.shadow.enabled=_o.shadows;s.shadow.near_plane=.025f;s.id=i+1;_scene.add_spot_light(s);}else if(point++<u32(_o.points))_scene.add_light({.position=d.position,.color=d.colour,.radius=d.radius});}
        _scene.taa.enabled=_o.taa;_scene.ambient_occlusion.enabled=_o.ao;_scene.ambient_occlusion.radius=2;_scene.ambient_occlusion.half_resolution=true;_scene.contact_shadows.enabled=_o.contact;
    }
    void load(SceneContext& ctx){
        ctx.renderer.set_job_system(_jobs);_r=std::make_unique<Renderer3D>(ctx.renderer);if(!_r->available())throw std::runtime_error("yard requires GPU backend");_data.load(_o.recipe);
        _o.points=_o.points<0?int(_data.point_count):_o.points;
        if(_o.points>int(_data.point_count))throw std::runtime_error("point-light count exceeds recipe");
        _o.props=_o.props<0?int(_data.prop_count):_o.props;_o.actors=_o.actors<0?int(_data.actor_count):_o.actors;_o.smoke=_o.smoke<0?int(_data.smoke_count):_o.smoke;
        if(_o.props>int(_data.prop_count)||_o.actors>int(_data.actor_count)||_o.smoke>int(_data.smoke_count))throw std::runtime_error("requested yard population exceeds recipe");
        if(_o.actors&&!_r->capabilities().skinning)throw std::runtime_error("yard skinning unavailable");
        if(_o.shadows&&!_r->capabilities().local_shadows)throw std::runtime_error("yard local shadows unavailable");
        _view=_r->create_view({1280*_o.scale,720*_o.scale},{.hdr=true});
        for(const auto& data:_data.meshes){auto mesh=_r->create_mesh(data);if(!mesh)throw std::runtime_error("yard mesh upload failed");_meshes.push_back(mesh);}
        std::vector<MeshData>().swap(_data.meshes);
        for(u32 i=0;i<5;++i){auto& t=_data.textures[i];_textures[i]=(i==1||i==2)?ctx.renderer.create_texture({int(t.side),int(t.side)},TextureFormat::Rgba8,t.rgba.data()):ctx.renderer.create_texture_from_rgba(t.rgba.data(),{int(t.side),int(t.side)});if(!_textures[i])throw std::runtime_error("yard texture upload failed");ctx.renderer.set_scale_mode(_textures[i],ScaleMode::Linear);std::vector<u8>().swap(t.rgba);}
        for(const auto& d:_data.materials){Material3D m;m.receives_shadow=_o.receives;m.color=d.colour;m.metallic=d.metallic;m.roughness=d.roughness;m.texture=_textures[0];m.metallic_roughness=_textures[1];m.occlusion=_textures[1];m.normal=_textures[2];m.normal_scale=.7f;m.emissive_texture=_textures[3];m.emissive=.15f;m.wrap_u=m.wrap_v=Wrap3D::Repeat;
            if(d.mode){m.blend=Blend3D::Alpha;m.double_sided=true;m.casts_shadow=false;if(d.mode==2){m.texture=_textures[4];m.unlit=true;m.normal={};m.emissive_texture={};m.emissive=0;}}
            _materials.push_back(m);}
        _environment=_r->create_environment({128,64},_data.sky);if(!_environment)throw std::runtime_error("yard environment failed");std::vector<f32>().swap(_data.sky);
        for(u32 mat=0;mat<_data.material_count;++mat){Dynamic d;d.material=mat;u32 mesh=0;for(int i=0;i<_o.props;++i){const auto& o=_data.props[i];if(o.moving&&o.material==mat){mesh=o.mesh;d.indices.push_back(i);d.records.push_back({o.transform,colors::white,u32(i+1)});}}
            if(d.records.empty())continue;if(_o.motion)_moving+=d.records.size();d.batch=InstanceBatch3D(_meshes[mesh],d.records,{{0,u32(d.records.size())}});_dynamic.push_back(std::move(d));}
        if(_o.structures)for(const auto& o:_data.world)if(_o.glass||o.material<_data.material_count)_triangles+=_data.triangles[o.mesh];
        for(int i=0;i<_o.props;++i)_triangles+=_data.triangles[_data.props[i].mesh];for(int i=0;i<_o.actors;++i)_triangles+=_data.triangles[_data.actors[i].mesh];_triangles+=u64(_o.smoke)*2;
        if(_o.preview)_frame=_o.warmup;_previous=Clock::now();
    }
    struct Dynamic{u32 material=0;std::vector<u32> indices;std::vector<MeshInstance> records;InstanceBatch3D batch;};
    Options _o;JobSystem* _jobs;yard::Recipe _data;std::vector<Mesh> _meshes;std::vector<Material3D> _materials;std::array<Texture,5> _textures;
    std::vector<Dynamic> _dynamic;std::vector<u32> _actor_joints;std::unique_ptr<Renderer3D> _r;View3D _view;Environment3D _environment;Scene3D _scene;
    std::ofstream _csv;Clock::time_point _previous;int _frame=0;u32 _route=0,_moving=0;u64 _triangles=0;std::array<double,3> _sums{};bool _ready=false;
};
}
int main(int argc,char** argv){try{
    Options o;std::vector<char*> runtime{argv[0]};
    for(int i=1;i<argc;++i){std::string a=argv[i];const auto value=[&](){if(++i==argc)throw std::runtime_error("missing yard option value");return std::string(argv[i]);};
        if(a=="--recipe")o.recipe=value();else if(a=="--metrics")o.metrics=value();else if(a=="--warmup")o.warmup=std::stoi(value());else if(a=="--measured")o.frames=std::stoi(value());else if(a=="--resolution-scale")o.scale=std::stoi(value());else if(a=="--point-lights")o.points=std::stoi(value());else if(a=="--workers")o.workers=std::stoi(value());else if(a=="--actors")o.actors=std::stoi(value());else if(a=="--smoke")o.smoke=std::stoi(value());else if(a=="--props")o.props=std::stoi(value());else if(a=="--route-offset")o.offset=std::stoi(value());else if(a=="--validate-recipe")o.validate=true;else if(a=="--visible")o.visible=true;else if(a=="--preview")o.preview=true;else if(a=="--gpu-pass-markers")o.gpu_pass_markers=true;else if(a=="--profile-cpu")o.profile=true;
        else if(a=="--receive-shadows"||a=="--animation"||a=="--motion"||a=="--glass"||a=="--structures"||a=="--local-shadows"||a=="--taa"||a=="--ao"||a=="--contact"){auto v=value();if(v!="0"&&v!="1")throw std::runtime_error("yard booleans require 0 or 1");bool* p=a=="--receive-shadows"?&o.receives:a=="--animation"?&o.animation:a=="--motion"?&o.motion:a=="--glass"?&o.glass:a=="--structures"?&o.structures:a=="--local-shadows"?&o.shadows:a=="--taa"?&o.taa:a=="--ao"?&o.ao:&o.contact;*p=v=="1";}
        else if(a=="--rebuild-scene")o.rebuild=true;
        else runtime.push_back(argv[i]);}
    if(o.validate){yard::Recipe recipe;recipe.load(o.recipe);std::cout<<"yard recipe validated: "<<recipe.prop_count<<" props, "<<recipe.source_triangles<<" source triangles\n";return 0;}
    if(o.recipe.empty()||o.metrics.empty()||o.warmup<0||o.warmup>300||o.frames<1||o.frames>6000||(o.scale!=1&&o.scale!=2)||o.points<-1||o.points>16||o.workers<0||o.workers>4||o.actors<-1||o.smoke<-1||o.props<-1||o.offset<0||o.offset>=1440)throw std::runtime_error("invalid yard options");
    auto headless=parse_headless_options(int(runtime.size()),runtime.data());headless.enabled=!o.preview&&!o.visible;headless.frames=o.visible&&!o.preview?0:o.warmup+o.frames+1;
    std::unique_ptr<JobSystem> jobs;if(o.workers)jobs=std::make_unique<JobSystem>(JobSystemConfig{.workers=o.workers,.max_background=1});
    SceneManager scenes;scenes.push(std::make_unique<Yard>(o,jobs.get()));return run_scene_app({.window={.title="Industrial yard",.width=1280,.height=720,.vsync=false,.max_fps=o.preview?60.f:0.f,.high_pixel_density=false},.headless=headless,.render_headless=true},scenes);
}catch(const std::exception& e){std::cerr<<"yard: "<<e.what()<<'\n';return 1;}}
