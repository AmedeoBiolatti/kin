#include <kin/ecs/scene3d.hpp>
#include <kin/anim/events.hpp>
#include <kin/core/instrumentation.hpp>
#include <chrono>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace kin;
void check(bool value,const char* why){if(!value)throw std::runtime_error(why);}
bool close(float a,float b){return std::abs(a-b)<1e-5f;}
Skeleton skeleton(){Skeleton s;s.names={"root","arm","hand"};s.parents={-1,0,1};s.rest.resize(3);s.rest[1].translation={0,1,0};s.inverse_bind.resize(3);return s;}
AnimClip3D clip(std::string name,float duration){AnimClip3D c{.name=std::move(name),.duration=duration};c.channels.resize(3);return c;}
AnimationMarker3D marker(float phase,std::string name){return {phase,{.channel="game",.name=std::move(name)}};}
void player() {
    auto sk=skeleton();auto base=clip("base",1),reach=clip("reach",.5f);
    base.channels[0].translation={.times={0,1},.values={{0,0,0},{2,0,0}}};
    reach.channels[0].translation={.times={0,.5f},.values={{99,0,0},{99,0,0}}};
    reach.channels[1].translation={.times={0,.5f},.values={{0,1,0},{0,3,0}}};
    AnimationEventTracks3D tracks{{"base",{marker(.25f,"step")}},
        {"reach",{marker(.5f,"contact")},MarkerSource3D::Action}};
    AnimationPlayer3D p{&sk};AnimationEventBatch3D batch;p.play(&base);
    auto a=p.play_action(&reach,{.fade_in=0,.fade_out=.1f,.mask_joint=1});
    check(bool(a)&&p.action_status(a)==ActionStatus3D::Playing,"action did not start");
    check(!p.play_action(&reach)&&p.current_action()==a,"busy action was replaced by default");
    p.update(.25f,tracks,batch);
    check(close(p.pose().joints[0].translation.x,.5f)&&close(p.pose().joints[1].translation.y,2),"action mask did not preserve moving base");
    check(batch.count==2&&batch.entries[0]->name=="step"&&!batch.actions[0]&&batch.entries[1]->name=="contact"&&batch.actions[1]==a,"source ownership/tie order wrong");
    p.update(.25f,tracks,batch);check(p.action_status(a)==ActionStatus3D::Finishing&&p.action_busy(),"action did not enter fade-out at end");
    p.update(.05f,tracks,batch);check(close(p.action_weight(),.5f),"natural fade-out weight wrong");
    p.update(.05f,tracks,batch);check(p.action_status(a)==ActionStatus3D::Completed&&!p.action_busy()&&close(p.pose().joints[1].translation.y,1),"action did not return to locomotion");
    const auto restarted=p.play_action(&reach,{.fade_in=.1f,.fade_out=.2f,.mask_joint=1});
    check(restarted!=a&&p.action_status(a)==ActionStatus3D::Superseded,"restart recycled a token");
    p.update(.05f);check(close(p.action_weight(),.5f),"fade-in weight wrong");
    check(p.cancel_action(restarted)&&p.action_status(restarted)==ActionStatus3D::Cancelled&&!p.action_event_valid(restarted),"cancel did not revoke event ownership");
    check(!p.cancel_action(restarted),"repeated cancel restarted fade-out");
    p.update(.1f);check(close(p.action_weight(),.25f),"cancel did not fade held pose from current weight");
    p.update(.1f);check(!p.action_busy()&&p.action_status(restarted)==ActionStatus3D::Cancelled,"cancelled action stayed busy");
    const auto rebinding=p.play_action(&reach,{.fade_in=0,.fade_out=.2f,.mask_joint=1});p.update(.1f);p.cancel_action(rebinding);
    p.set_skeleton(&sk);check(!p.action_busy()&&!p.action_event_valid(rebinding),"rebind kept an already cancelling clip alive");
    const auto token=p.play_action(&reach,{.fade_in=0,.fade_out=.1f,.mask_joint=1});p.update(.25f,tracks,batch);
    const auto replacement=p.play_action(&reach,{.fade_in=0,.mask_joint=1},ActionStart3D::Replace);
    check(bool(replacement)&&replacement!=token&&!p.action_event_valid(token)&&p.action_time()==0,"replace did not invalidate pending old events/restart clock");
    const auto before=p.current_action();
    check(!p.play_action(nullptr,{},ActionStart3D::Replace)&&!p.play_action(&reach,{.speed=0},ActionStart3D::Replace)&&
          !p.play_action(&reach,{.fade_in=-1},ActionStart3D::Replace)&&!p.play_action(&reach,{.mask_joint=5},ActionStart3D::Replace)&&p.current_action()==before,"invalid request destroyed current action");
    check(!p.cancel_action(replacement,-.5f)&&p.action_status(replacement)==ActionStatus3D::Playing,"invalid cancel mutated playback");
    p.cancel_action(replacement,0);p.play(&base);
    const auto full=p.play_action(&reach,{.fade_in=0,.fade_out=0,.base_events=false});p.update(.75f,tracks,batch);
    check(batch.count==1&&batch.entries[0]->name=="contact"&&batch.actions[0]==full&&p.action_status(full)==ActionStatus3D::Completed&&p.action_event_valid(full),"full action ownership/completion overshoot wrong");
    check(p.cancel_action(full,0)&&!p.action_event_valid(full),"cancel did not revoke undelivered markers from completed action");
    p.play(&base);const auto layered=p.play_action(&reach,{.fade_in=0,.mask_joint=1},ActionStart3D::Replace);
    tracks[1].markers={marker(.2f,"early"),marker(.5f,"contact")};p.update(.5f,tracks,batch);
    check(batch.count==3&&batch.entries[0]->name=="early"&&batch.entries[1]->name=="step"&&batch.entries[2]->name=="contact","mixed source events were not ordered in step time");
    p.set_skeleton(&sk);check(!p.action_event_valid(layered)&&p.action_status(layered)==ActionStatus3D::Cancelled,"rebind left action events active");
    PoseMask mask;mask.set_subtree(sk,1,.7f,false);check(mask.weights==PoseMask::subtree(sk,1,.7f,false).weights&&mask.weights[1]==0&&mask.weights[2]==.7f,"reusable subtree differs from existing mask selection");
    AnimationPlayer3D scope{&sk};scope.play(&reach,false);scope.update(.25f,tracks,batch);
    check(batch.count==0,"action-only markers leaked through base clip");
    tracks[1].source=MarkerSource3D::Both;scope.set_time(0);scope.update(.25f,tracks,batch);
    check(batch.count==2&&!batch.actions[0]&&!batch.actions[1],"explicit both-source markers did not reach base");
    tracks[1].source=MarkerSource3D::Action;
    base.channels[1].translation={.times={0,1},.values={{0,4,0},{0,4,0}}};
    p.play(&base);p.play_action(&reach,{.fade_in=0,.mask_joint=1,.additive=true},ActionStart3D::Replace);p.update(.25f);
    check(close(p.pose().joints[1].translation.y,5),"additive action did not add change from its reference pose");
    base.channels[1].translation={};
    p.play(&base);p.play_action(&reach,{.fade_in=0,.mask_joint=1},ActionStart3D::Replace);p.update(.01f,tracks,batch);
    {AllocationScope guard{true};for(int i=0;i<10000;++i) {
        p.play_action(&reach,{.fade_in=0,.mask_joint=1},ActionStart3D::Replace);p.update(.01f,tracks,batch);
    }}
}
struct Scratch {
    std::filesystem::path path=std::filesystem::temp_directory_path()/("kin-action-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Scratch(){std::filesystem::create_directories(path);}~Scratch(){std::error_code e;std::filesystem::remove_all(path,e);}
};
void assets_and_dispatch() {
    Scratch scratch;const auto path=scratch.path/"actor.kinanimator";
    const auto write=[&](std::string_view actions,std::string_view source="action") {
        std::ofstream(path)<<"{\"schema\":\"kin.animator3d/1\",\"actions\":"<<actions<<",\"events\":[{\"clip\":\"reach\",\"source\":\""<<source
          <<"\",\"markers\":[{\"phase\":0.2,\"channel\":\"game\",\"name\":\"first\"},{\"phase\":0.4,\"channel\":\"game\",\"name\":\"second\"}]}]}";
    };
    constexpr auto config=R"([{"name":"pickup","clip":"reach","mask_joint":"arm","fade_in":0,"fade_out":0.05}])";
    write(config);kin::EcsWorld world;register_render3d_components(world);WorldRenderState3D state{world};
    auto model=std::make_shared<Model>();model->skeletons.push_back(skeleton());model->clips={clip("base",1),clip("reach",.5f)};
    Animator3D setup;setup.clip="base";
    auto actor=world.entity().set(SkinnedMeshRenderer{.model=model}).set(setup).set(AnimatorAsset3D{.asset={"actor.kinanimator"}});
    auto other=world.entity().set(SkinnedMeshRenderer{.model=model}).set(setup).set(AnimatorAsset3D{.asset={"actor.kinanimator"}});
    check(resolve_animator3d_assets(world,scratch.path).empty(),"valid action asset rejected");
    check(actor.get<Animator3D>()->actions==other.get<Animator3D>()->actions,"action definitions copied per instance");
    const auto a=play_action3d(actor,"pickup");check(bool(a),"could not start before first animate");
    state.animate(.25f);check(action_status3d(actor,a)==ActionStatus3D::Playing&&actor.get<Animator3D>()->events.count==2,"first bind reset action or omitted scoped events");
    int calls=0;AnimationEventDispatch dispatch;
    dispatch.on("game",[&](kin::EcsEntity e,const AnimationEvent&){++calls;cancel_action3d(e,a,0);});
    check(dispatch_animation_events3d(world,dispatch)==0&&calls==1,"cancel in callback did not suppress later events from same action");
    check(dispatch_animation_events3d(world,dispatch)==0&&calls==1,"dispatch replayed action event");
    const auto b=play_action3d(actor,"pickup");state.animate(.25f);const auto c=play_action3d(actor,"pickup",ActionStart3D::Replace);
    dispatch_animation_events3d(world,dispatch);check(bool(b)&&bool(c)&&calls==1,"restart delivered stale batch");
    auto shared=actor.get<Animator3D>()->actions;
    check(resolve_animator3d_assets(world,scratch.path).empty()&&actor.get<Animator3D>()->player.current_action()==c,"unchanged source reset action");
    for(const auto* invalid:{R"([{"name":"pickup","clip":"missing"}])",R"([{"name":"pickup","clip":"reach","mask_joint":"missing"}])",
        R"([{"name":"pickup","clip":"reach","speed":0}])",R"([{"name":"pickup","clip":"reach","weight":2}])",
        R"([{"name":"x","clip":"reach"},{"name":"x","clip":"reach"}])",R"([{"name":"x","clip":"reach","bogus":true}])"}) {
        write(invalid);check(!resolve_animator3d_assets(world,scratch.path).empty()&&actor.get<Animator3D>()->actions==shared&&actor.get<Animator3D>()->player.current_action()==c,"invalid action config replaced live state");
    }
    write(config,"unknown");check(!resolve_animator3d_assets(world,scratch.path).empty(),"unknown marker source accepted");
    write("[]");check(resolve_animator3d_assets(world,scratch.path).empty()&&!actor.get<Animator3D>()->actions&&action_status3d(actor,c)==ActionStatus3D::Superseded,"reload retained old action");
    write(config);check(resolve_animator3d_assets(world,scratch.path).empty(),"action config restore failed");
    const auto d=play_action3d(other,"pickup");check(bool(d)&&d!=c&&action_status3d(other,c)==ActionStatus3D::Superseded,"tokens aliased across actors");
    state.animate(.25f);other.remove<SkinnedMeshRenderer>();state.animate(.1f);
    check(action_status3d(other,d)==ActionStatus3D::Cancelled&&other.get<Animator3D>()->events.count==0,"renderer removal left action alive");
    check(!play_action3d(actor,"missing")&&!play_action3d({},"pickup"),"unknown action/invalid actor accepted");
}
}
int main(){try{player();assets_and_dispatch();std::cout<<"one-shot masks, lifecycle, token ownership, event ordering, asset reload and 10000 warmed restarts passed; player_bytes="<<sizeof(kin::AnimationPlayer3D)<<" batch_bytes="<<sizeof(kin::AnimationEventBatch3D)<<'\n';}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
