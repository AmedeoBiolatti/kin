#include <kin/ecs/scene3d.hpp>
#include <kin/anim/events.hpp>
#include <kin/core/instrumentation.hpp>
#include <fstream>
#include <chrono>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace kin;
void check(const auto& condition,const char* why) {if(!condition)throw std::runtime_error(why);}
Skeleton3D skeleton() {Skeleton3D s;s.names={"root"};s.parents={-1};s.rest.resize(1);s.inverse_bind.resize(1);return s;}
AnimClip3D clip(std::string name,float duration=1) {return {.name=std::move(name),.duration=duration};}
AnimationMarker3D marker(float phase,std::string name) {return {phase,{.channel="game",.name=std::move(name)}};}
void player_events() {
    auto sk=skeleton();auto a=clip("a"),b=clip("b",2);
    AnimationEventTracks3D tracks{{"a",{marker(.75f,"right"),marker(0,"zero"),marker(.25f,"left")}},
                                 {"b",{marker(.25f,"other"),marker(.75f,"other-right")}}};
    AnimationPlayer3D player{&sk};AnimationEventBatch3D out;
    player.play(&a);player.update(.25f,tracks,out);
    check(out.count==1&&out.entries[0]->name=="left","exact forward endpoint/initial phase zero wrong");
    player.update(.5f,tracks,out);check(out.count==1&&out.entries[0]->name=="right","marker repeated at beginning of interval");
    player.update(.5f,tracks,out);check(out.count==2&&out.entries[0]->name=="zero"&&out.entries[1]->name=="left","wrap event order wrong");
    player.update(0,tracks,out);check(out.count==0&&out.dropped==0,"zero dt retained output");
    player.set_time(.7f);player.update(.05f,tracks,out);check(out.count==1&&out.entries[0]->name=="right","seek emitted skipped events or missed endpoint");
    player.play(&a,true,.5f);player.set_speed(-1);player.update(.25f,tracks,out);
    check(out.count==1&&out.entries[0]->name=="left","reverse endpoint wrong");
    player.update(.5f,tracks,out);check(out.count==2&&out.entries[0]->name=="zero"&&out.entries[1]->name=="right","reverse wrap/order wrong");
    player.set_speed(1);player.play(&a,false);player.update(3,tracks,out);
    check(out.count==2&&out.entries[0]->name=="left"&&out.entries[1]->name=="right","nonloop emitted wraps");
    player.update(3,tracks,out);check(out.count==0,"finished nonloop repeated events");
    player.set_speed(-1);player.update(3,tracks,out);
    check(out.count==3&&out.entries[2]->name=="zero","nonloop reverse stop wrong");
    player.set_speed(1);player.play(&a);player.crossfade_to(&b,1);player.update(.5f,tracks,out);
    check(out.count==1&&out.entries[0]->name=="other","fade emitted outgoing events");
    player.crossfade_to(&a,1);player.update(.25f,tracks,out);
    check(out.count==1&&out.entries[0]->name=="left","interrupted fade duplicated events");
    const BlendPoint3D points[]{{&a,0},{&b,1}};
    player.play_blend(points,.5f);player.update(.375f,tracks,out);
    check(out.count==1&&out.entries[0]->name=="other","blend tie/duration ownership wrong");
    player.set_blend_parameter(.49f);player.update(.01f,tracks,out);check(out.count==0,"dominant switch replayed marker");
    player.layer(0).clip=&a;player.play(&b);player.update(.5f,tracks,out);
    check(out.count==1&&out.entries[0]->name=="other","pose layer emitted duplicate events");
    player.clear_layers();player.play(&a);player.update(10,tracks,out);
    check(out.count==16&&out.dropped==14,"multi-loop overflow count wrong");
    check(out.entries[0]->name=="left"&&out.entries[1]->name=="right"&&out.entries[2]->name=="zero","overflow kept wrong prefix");
    player.play(&a);player.set_speed(-1);player.update(10,tracks,out);
    check(out.count==16&&out.dropped==14&&out.entries[0]->name=="right"&&out.entries[1]->name=="left","reverse overflow wrong");
    player.set_speed(1);player.play(&a);player.update(1e30f,tracks,out);
    check(out.count==16&&out.dropped==UINT32_MAX,"huge step did not saturate/drop");
    player.play(&a);player.update(std::numeric_limits<float>::infinity(),tracks,out);
    check(out.count==0&&player.time()==0,"nonfinite dt advanced clock");
    player.update(-1,tracks,out);check(out.count==0&&player.time()==0,"negative dt advanced clock");
    player.update(.1f,tracks,out);
    {AllocationScope guard{true};for(int i=0;i<10000;++i)player.update(.01f,tracks,out);}
}
struct Scratch {
    std::filesystem::path path=std::filesystem::temp_directory_path()/("kin-marker-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Scratch(){std::filesystem::create_directories(path);}
    ~Scratch(){std::error_code error;std::filesystem::remove_all(path,error);}
};
void ecs_assets() {
    Scratch temp;const auto file=temp.path/"actor.kinanimator";
    const auto write=[&](const std::string& events){std::ofstream(file)<<"{\"schema\":\"kin.animator3d/1\",\"events\":"<<events<<"}";};
    write(R"([{"clip":"a","markers":[{"phase":0.25,"channel":"game","name":"hit"}]}])");
    kin::EcsWorld world;register_render3d_components(world);WorldRenderState3D rendering{world};
    auto model=std::make_shared<Model3D>();model->skeletons.push_back(skeleton());model->clips.push_back(clip("a"));
    auto actor=world.entity().set(Transform3D{}).set(SkinnedMeshRenderer3D{.model=model}).set(Animator3D{.clip="a"})
        .set(AnimatorAsset3D{.asset={"actor.kinanimator"}});
    auto other=world.entity().set(SkinnedMeshRenderer3D{.model=model}).set(AnimatorAsset3D{.asset={"actor.kinanimator"}});
    check(resolve_animator3d_assets(world,temp.path).diagnostics.empty(),"valid event asset rejected");
    check(actor.get<Animator3D>()->event_tracks==other.get<Animator3D>()->event_tracks,"marker data copied per instance");
    rendering.animate(.25f);check(actor.get<Animator3D>()->events.count==1,"ECS animation omitted markers");
    AnimationEventDispatch dispatch;int hits=0;
    dispatch.on("game",[&](kin::EcsEntity e,const AnimationEvent& event){check(e.id()==actor.id()&&event.name=="hit","wrong event source/payload");++hits;});
    check(dispatch_animation_events3d(world,dispatch)==0&&hits==1,"event bridge failed");
    check(dispatch_animation_events3d(world,dispatch)==0&&hits==1,"dispatch repeated output");
    rendering.animate(1);AnimationEventDispatch unknown;
    check(dispatch_animation_events3d(world,unknown)==1&&actor.get<Animator3D>()->events.count==0,"unknown channel accumulated");
    rendering.animate(1);actor.get_mut<Animator3D>()->playing=false;rendering.animate(.1f);
    check(actor.get<Animator3D>()->events.count==0,"paused animator retained events");
    actor.get_mut<Animator3D>()->playing=true;rendering.animate(1);
    auto current=actor.get<Animator3D>()->event_tracks;const auto time=actor.get<Animator3D>()->player.time();
    check(resolve_animator3d_assets(world,temp.path).diagnostics.empty()&&actor.get<Animator3D>()->player.time()==time,"unchanged config restarted player");
    for(const auto* bad:{R"([{"clip":"a","markers":[{"phase":1,"channel":"game","name":"bad"}]}])",
                         R"([{"clip":"missing","markers":[]}])",R"([{"clip":"a","markers":[{"channel":"game","name":"bad"}]}])",
                         R"([{"clip":"a","markers":[{"phase":0.3,"channel":"","name":"bad"}]}])",
                         R"([{"clip":"a","markers":[],"bogus":true}])"}) {
        write(bad);check(!resolve_animator3d_assets(world,temp.path).diagnostics.empty()&&actor.get<Animator3D>()->event_tracks==current,"invalid config replaced good markers");
    }
    std::string large="[{\"clip\":\"a\",\"markers\":[";
    for(int i=0;i<257;++i){if(i)large+=',';large+=R"({"phase":0.5,"channel":"game","name":"x"})";}large+="]}]";
    write(large);check(!resolve_animator3d_assets(world,temp.path).diagnostics.empty(),"oversized markers accepted");
    write("[]");check(resolve_animator3d_assets(world,temp.path).diagnostics.empty()&&!actor.get<Animator3D>()->event_tracks&&actor.get<Animator3D>()->events.count==0,"reload retained stale event references");
    auto native=std::make_shared<AnimationEventTracks3D>(AnimationEventTracks3D{{"a",{marker(.25f,"hit")}}});
    std::weak_ptr<const AnimationEventTracks3D> retained=native;
    actor.get_mut<Animator3D>()->event_tracks=native;native.reset();
    rendering.animate(.25f);check(actor.get<Animator3D>()->events.count==1,"native table did not emit");
    actor.get_mut<Animator3D>()->event_tracks.reset();
    check(!retained.expired(),"pending native table replacement invalidated event references");
    const auto before_hits=hits;
    check(dispatch_animation_events3d(world,dispatch)==0&&hits==before_hits+1&&retained.expired(),"batch did not retain/release its original table through dispatch");
    actor.get_mut<Animator3D>()->events.count=1;actor.get_mut<SkinnedMeshRenderer3D>()->model.reset();rendering.animate(.1f);
    check(actor.get<Animator3D>()->events.count==0,"missing model retained events");
    actor.get_mut<Animator3D>()->events.count=1;actor.remove<SkinnedMeshRenderer3D>();rendering.animate(.1f);
    check(actor.get<Animator3D>()->events.count==0&&dispatch_animation_events3d(world,unknown)==0,"removed renderer retained stale events");
}
}
int main() {
    try{player_events();ecs_assets();std::cout<<"animation marker crossing, blending, bounded output, dispatch and assets passed; batch_bytes="<<sizeof(kin::AnimationEventBatch3D)<<" per_animator_event_bytes="<<(sizeof(kin::AnimationEventBatch3D)+sizeof(std::shared_ptr<const kin::AnimationEventTracks3D>))<<'\n';}
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
