#include "../examples/camp_walk/gameplay.hpp"
#include <kin/platform/app.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <cstdlib>
#include <iostream>

namespace {
using namespace kin;
namespace camp=camp_walk;
void check(const auto& value,const char* why){if(!value)throw std::runtime_error(why);}
SceneSession3DOptions options() {
    SceneSession3DOptions options;options.viewport={160,120};options.view.samples=1;options.warm_up=false;
    options.physics=World3DConfig{.max_bodies=128,.max_body_pairs=512,.max_contact_constraints=512,.temp_memory_bytes=2u*1024u*1024u,.single_threaded=true,.max_events=256};
    options.max_runtime_prefabs=8;options.max_runtime_entities=16;options.max_spawn_entities=4;options.max_pending_commands=8;
    options.register_components=camp::register_components;options.initialize=camp::install;return options;
}
struct Result { camp::Progress progress;int collected;Vec3f position; };
Result route(Renderer3D& renderer,Assets3D& assets,int device) {
    SceneSession3D session{renderer,assets,options()};check(session.load("scenes/camp-play.kinscene")&&camp::prepare(session),"camp load failed");
    Input input;input.set_map(camp::input_map());camp::InputMapper mapper;
    for(int i=0;i<239;++i) {
        const auto demo=camp::demo_commands(i);camp::PlayerCommand command=demo.player;
        if(device==1) {
            input.begin_frame();
            input.set_action_held("camp.left",i>=60&&i<90);input.set_action_held("camp.forward",i>=100&&i<130);
            input.set_action_held("camp.jump",i==150);input.set_action_held("camp.crouch",i>=190&&i<210);
            input.set_action_held("camp.interact",i==40||i==100||i==140);input.set_action_held("camp.push",i==175);
            command=mapper.sample(input,{},1.f/60).player;input.advance_step_edges();
        } else if(device==2) {
            GamepadState pad;pad.connected=true;
            pad.left_x=demo.player.move.x;pad.left_y=demo.player.move.y;
            pad.set_button(GamepadButton::A,demo.player.jump);pad.set_button(GamepadButton::B,demo.player.crouch);
            pad.set_button(GamepadButton::X,demo.player.interact);pad.set_button(GamepadButton::Y,demo.player.push);
            command=mapper.sample(input,pad,1.f/60).player;
        }
        if(demo.restart)check(camp::restart(session),"camp route restart failed");
        camp::submit(*session.world(),command);
        if(!session.step(1.f/60)){for(const auto& e:session.diagnostics())std::cerr<<e<<'\n';check(false,"camp route step failed");}
        if(device==1) {
            const auto hash=session.physics()->state_hash();const auto pose=session.entity("player").get<Transform3D>()->matrix();
            for(int render=0;render<3;++render)check(camp::present(session,float(render)/2,.005f),"camp presentation failed");
            check(session.physics()->state_hash()==hash&&session.entity("player").get<Transform3D>()->matrix()==pose,"camp presentation changed simulation");
        }
        check(session.runtime_prefabs()<=5,"camp population grew");
    }
    const auto& game=camp::state(*session.world());const auto& p=game.progress;
    check(camp::demo_passed(game)&&game.collected==3&&session.runtime_prefabs()==2,"command route did not finish gameplay");
    check(p.steps==239&&p.input_steps==p.steps&&p.movement_steps==p.steps&&p.interaction_steps==p.steps&&p.camera_steps==p.steps,"game phases skipped or duplicated across reset");
    check(p.spawned_total==10&&p.level_resets==1&&p.collected_total==3&&p.rounds_completed==1&&p.pushes==1,"game counters differ from expected route");
    check(p.pickups_started==3&&p.pickup_contacts==3&&p.pickup_awards==3&&p.pickups_cancelled==0,"demo actions did not complete exactly once");
    check(p.footsteps>0&&p.animation_events_dropped==0&&p.animation_events_unhandled==0,"camp marker events missing or overflowing");
    const auto footsteps=p.footsteps;
    const auto hash=session.physics()->state_hash();check(camp::present(session,.5f,.01f)&&session.collect(),"camp lantern collect failed");
    const auto lantern=session.resolve(game.lantern.root);const auto* attached=lantern.get<JointAttachment3D>();
    check(attached&&attached->status()==JointAttachmentStatus3D::Ready&&attached->placement()&&session.physics()->state_hash()==hash,"lantern did not follow rendered hand without moving physics");
    check(p.footsteps==footsteps&&p.pickup_contacts==3,"rendering emitted gameplay markers");
    return {p,game.collected,session.entity("player").get<CharacterController3D>()->state.position};
}
void boundaries(Renderer3D& renderer,Assets3D& assets) {
    SceneSession3D session{renderer,assets,options()};check(session.load("scenes/camp-play.kinscene")&&camp::prepare(session),"boundary load failed");
    check(session.step(1.f/60),"initial controller pose step failed");
    const auto old=camp::state(*session.world()).supplies[1];auto supply=session.resolve(old.root);
    // Before physics, the published player pose is still at spawn and too far
    // away. A teleport is applied by native physics before interaction selects.
    check(length_squared(session.entity("player").get<CharacterController3D>()->state.position-supply.get<Transform3D>()->position)>1.25f*1.25f,"fresh-pose fixture is too close");
    session.entity("player").get_mut<Transform3D>()->position={-.8f,0,.8f};
    camp::submit(*session.world(),{.interact=true});check(session.step(1.f/60),"fresh-pose interaction failed");
    check(camp::state(*session.world()).collected==0&&session.alive(old)&&camp::state(*session.world()).pickup.target==old.root,"pickup did not select fresh pose/delay award until contact");
    // The amount is read at contact, not captured when the button was pressed.
    supply.get_mut<camp::Supply>()->amount=2;
    for(int i=0;i<12;++i)check(session.step(1.f/60),"delayed pickup failed");
    check(camp::state(*session.world()).collected==2&&!session.alive(old)&&session.runtime_prefabs()==4,"contact did not commit/award pickup");
    check(session.step(1.f/60)&&camp::state(*session.world()).collected==2,"unsubmitted one-shot action repeated");
    camp::submit(*session.world(),{.move={1,0},.look={90,500},.jump=true});check(session.step(1.f/60),"look/movement command failed");
    const auto forward=session.entity("player").get<CharacterController3D>()->input.forward;
    check(forward.x>.99f&&std::abs(forward.z)<.0001f&&camp::state(*session.world()).camera.pitch()==-5,"look did not rotate movement/clamp pitch");
    check(session.step(1.f/60)&&session.entity("player").get<CharacterController3D>()->input.move.x==0&&!session.entity("player").get<CharacterController3D>()->input.jump,"missing command repeated held movement/jump");
    const auto before=camp::state(*session.world()).progress.steps;
    camp::submit(*session.world(),{.interact=true});check(camp::restart(session)&&session.step(1.f/60),"restart command reset failed");
    check(camp::state(*session.world()).collected==0&&camp::state(*session.world()).progress.steps==before+1&&session.runtime_prefabs()==5,"restart carried stale pending actions/ownership");
    const auto intact=camp::state(*session.world()).progress.steps;
    check(!session.prepare("missing-camp.kinscene")&&camp::state(*session.world()).progress.steps==intact&&session.runtime_prefabs()==5,"failed preparation mutated active game");
    session.entity("player").get_mut<Transform3D>()->position={10,-5,10};
    check(session.step(1.f/60)&&camp::state(*session.world()).restart_requested,"fall did not request scene restart");
    check(camp::restart(session)&&!camp::state(*session.world()).restart_requested&&session.runtime_prefabs()==5,"fall restart did not reset game state/population");
    // A refused native teardown must never award a pickup.
    const auto victim=camp::state(*session.world()).supplies[0];auto entity=session.resolve(victim.root);
    session.world()->entity("foreign").child_of(entity);
    session.entity("player").get_mut<Transform3D>()->position={1.6f,0,1.4f};
    camp::submit(*session.world(),{.interact=true});
    check(session.step(1.f/60)&&session.alive(victim)&&camp::state(*session.world()).collected==0,"pickup request removed supply early");
    bool failed=false;for(int i=0;i<15&&!failed;++i)failed=!session.step(1.f/60);
    check(failed&&session.alive(victim)&&camp::state(*session.world()).collected==0,"failed contact removal awarded collection");
}
void visibility(Renderer3D& renderer,Assets3D& assets) {
    SceneSession3D session{renderer,assets,options()};check(session.load("scenes/camp-play.kinscene")&&camp::prepare(session),"visibility load failed");
    const auto owned=camp::state(*session.world()).supplies;
    check(session.despawn(owned[1])&&session.despawn(owned[2]),"visibility fixture setup failed");
    for(int i=0;i<10;++i)check(session.step(1.f/60),"visibility grounding failed");
    check(camp::state(*session.world()).nearby==0&&session.resolve(owned[0].root).get<PointLight3D>()->color.r>1,"visible supply was not highlighted");
    const auto wall_shape=Shape3D::box({.06f,.5f,.5f});
    auto wall=session.physics()->create_body({.shape=wall_shape,.motion=Body3DMotion::Static,.position={1.3f,.5f,1.2f}});
    camp::submit(*session.world(),{.interact=true});check(session.step(1.f/60),"hand occlusion step failed");
    check(camp::state(*session.world()).nearby<0&&camp::state(*session.world()).collected==0&&session.alive(owned[0])&&session.resolve(owned[0].root).get<PointLight3D>()->color.r<1,"unmapped wall did not block collection/clear highlight");
    session.physics()->destroy_body(wall);check(session.step(1.f/60)&&camp::state(*session.world()).nearby==0,"cleared hand occlusion did not restore selection");
    const auto camera_position=session.entity("camera").get<Transform3D>()->position;
    const auto supply_position=session.resolve(owned[0].root).get<Transform3D>()->position+Vec3f{0,.3f,0};
    wall=session.physics()->create_body({.shape=Shape3D::box({.08f,.08f,.08f}),.motion=Body3DMotion::Static,.position=lerp(camera_position,supply_position,.65f)});
    camp::submit(*session.world(),{.interact=true});check(session.step(1.f/60)&&camp::state(*session.world()).nearby<0&&camp::state(*session.world()).collected==0,"camera-only occlusion allowed collection");
    session.physics()->destroy_body(wall);
    std::array<Body3D,9> triggers;const auto small=Shape3D::sphere(.03f);
    for(size_t i=0;i<triggers.size();++i)triggers[i]=session.physics()->create_body({.shape=small,.motion=Body3DMotion::Static,.position={1+.03f*float(i),.3f,1},.layer=layers3d::Sensor,.sensor=true});
    camp::submit(*session.world(),{.interact=true});check(session.step(1.f/60)&&camp::state(*session.world()).query_overflow&&camp::state(*session.world()).query_overflows>0&&camp::state(*session.world()).nearby<0&&camp::state(*session.world()).collected==0,"query saturation did not fail closed");
    for(auto body:triggers)session.physics()->destroy_body(body);
    camp::submit(*session.world(),{.interact=true});check(session.step(1.f/60)&&!camp::state(*session.world()).query_overflow&&camp::state(*session.world()).collected==0&&session.alive(owned[0]),"unblocked pickup did not start with delayed award");
    for(int i=0;i<12;++i)check(session.step(1.f/60),"unblocked pickup contact failed");
    check(camp::state(*session.world()).collected==1&&!session.alive(owned[0]),"unblocked collection did not recover");
}

void pushing(Renderer3D& renderer,Assets3D& assets) {
    SceneSession3D session{renderer,assets,options()};check(session.load("scenes/camp-play.kinscene")&&camp::prepare(session),"push scene failed");
    for(int i=0;i<12;++i)check(session.step(1.f/60),"push grounding failed");
    session.entity("player").get_mut<Transform3D>()->position={2.8f,0,2.8f};
    camp::submit(*session.world(),{.push=true});check(session.step(1.f/60),"distant push step failed");
    check(!camp::state(*session.world()).push_focus&&camp::state(*session.world()).progress.pushes==0,"distant crate could be pushed");
    session.entity("player").get_mut<Transform3D>()->position={1.15f,0,.6f};
    check(session.step(1.f/60)&&bool(camp::state(*session.world()).push_focus),"nearby crate did not offer push verb");
    const auto wall=session.physics()->create_body({.shape=Shape3D::box({.08f,.5f,.08f}),.motion=Body3DMotion::Static,.position={1.15f,.4f,0}});
    camp::submit(*session.world(),{.push=true});check(session.step(1.f/60),"occluded push step failed");
    check(!camp::state(*session.world()).push_focus&&camp::state(*session.world()).progress.pushes==0,"occluded crate could be pushed");
    session.physics()->destroy_body(wall);
    camp::submit(*session.world(),{.push=true});check(session.step(1.f/60),"clear push step failed");
    check(camp::state(*session.world()).push_focus.verb==camp::Push&&camp::state(*session.world()).progress.pushes==1,"clear push did not apply exactly once");
    check(session.step(1.f/60)&&camp::state(*session.world()).progress.pushes==1,"push repeated without a command");
    check(camp::interaction_prompt(camp::Collect)=="E collect"&&camp::interaction_prompt(camp::Push)=="F push"&&
        camp::interaction_prompt(camp::Collect,true)=="X collect"&&camp::interaction_prompt(camp::Push,true)=="Y push","native verb prompts incorrect");
}

void interruption(Renderer3D& renderer,Assets3D& assets,int mode) {
    SceneSession3D session{renderer,assets,options()};check(session.load("scenes/camp-play.kinscene")&&camp::prepare(session),"interruption scene failed");
    for(int i=0;i<12;++i)check(session.step(1.f/60),"interruption grounding failed");
    const auto target=camp::state(*session.world()).supplies[0];
    camp::submit(*session.world(),{.interact=true});check(session.step(1.f/60),"pickup action did not start");
    const auto token=camp::state(*session.world()).pickup_token;
    check(bool(token)&&camp::state(*session.world()).pickup.target==target.root&&session.alive(target)&&camp::state(*session.world()).collected==0,"pickup target/award timing wrong");
    for(int r=0;r<3;++r)check(camp::present(session,float(r)/2,.01f)&&session.collect(),"pickup render failed");
    check(camp::state(*session.world()).progress.pickup_contacts==0,"rendering emitted pickup contact");
    auto player=session.entity("player");Body3D wall;
    if(mode==0)player.get_mut<Transform3D>()->position={2.5f,0,2.5f};
    if(mode==1)check(session.despawn(target),"target despawn failed");
    if(mode==2)wall=session.physics()->create_body({.shape=Shape3D::box({.06f,.5f,.5f}),.motion=Body3DMotion::Static,.position={1.3f,.5f,1.2f}});
    if(mode==3)camp::submit(*session.world(),{.jump=true});
    if(mode==4)check(bool(play_action3d(player,"pickup",ActionStart3D::Replace)),"replacement failed");
    std::array<Body3D,9> triggers{};
    if(mode==6)for(size_t i=0;i<triggers.size();++i)triggers[i]=session.physics()->create_body({.shape=Shape3D::sphere(.03f),.motion=Body3DMotion::Static,.position={1+.03f*float(i),.3f,1},.layer=layers3d::Sensor,.sensor=true});
    if(mode==7) {
        auto tracks=std::make_shared<AnimationEventTracks3D>();
        ClipEventTrack3D noisy{.clip="Idle"};
        const auto phase=player.get<Animator3D>()->player.time()+.0001f;
        for(int i=0;i<20;++i)noisy.markers.push_back({phase,{.channel="camp",.name="noise"}});
        tracks->push_back(std::move(noisy));player.get_mut<Animator3D>()->event_tracks=std::move(tracks);
    }
    if(mode==5) {
        check(camp::restart(session)&&!session.resolve(target.root)&&action_status3d(session.entity("player"),token)==ActionStatus3D::Superseded,"restart retained pending action/target identity");
    }
    for(int i=0;i<40;++i)check(session.step(1.f/60),"interrupted pickup step failed");
    const auto& state=camp::state(*session.world());
    check(state.collected==0&&state.progress.pickup_awards==0&&!state.pickup.target&&state.progress.pickups_cancelled==1,"interruption awarded/retained pickup");
    if((mode<3||mode==6)&&state.progress.pickup_contacts!=1) {
        std::cerr<<"interruption mode="<<mode<<" contacts="<<state.progress.pickup_contacts<<" grounded="<<state.progress.grounded<<" airborne="<<state.progress.airborne<<" ground="<<int(player.get<CharacterController3D>()->state.ground)<<" y="<<player.get<CharacterController3D>()->state.position.y<<'\n';
        check(false,"contact revalidation was bypassed by an early cancellation");
    }
    if(mode==6)check(state.query_overflows>0,"contact overflow fixture did not saturate");
    if(mode==7)check(state.progress.animation_events_dropped>=4,"animation event overflow fixture did not saturate");
    if(mode!=1&&mode!=5)check(session.alive(target),"cancelled pickup destroyed target");
    if(wall)session.physics()->destroy_body(wall);
    for(auto body:triggers)if(body)session.physics()->destroy_body(body);
}

}
int main(int argc,char** argv) {
    bool ready=false;
    try {
        App app;auto& window=app.create_window({.title="camp-gameplay-tests",.width=160,.height=120,.hidden=true});
        auto backend=make_render_backend(window,false,true);if(!backend||backend->name()!="SDL_GPU")throw std::runtime_error("GPU backend required");
        ready=true;Renderer2D renderer{std::move(backend)};Renderer3D r3d{renderer};Assets3D assets{r3d,KIN_TEST_SUMMIT};
        if(argc==2&&std::string_view(argv[1])=="push"){pushing(r3d,assets);return 0;}
        if(argc>1) {
            check(argc==3&&std::string_view(argv[1])=="action","invalid test mode");const int mode=std::stoi(argv[2]);
            check(mode>=0&&mode<8,"invalid action interruption index");interruption(r3d,assets,mode);return 0;
        }
        const auto scripted=route(r3d,assets,0),keyboard=route(r3d,assets,1),pad=route(r3d,assets,2);
        check(length_squared(scripted.position-keyboard.position)<1e-10f&&length_squared(scripted.position-pad.position)<1e-10f&&scripted.progress.grounded==keyboard.progress.grounded&&scripted.progress.grounded==pad.progress.grounded&&scripted.progress.footsteps==keyboard.progress.footsteps&&scripted.progress.footsteps==pad.progress.footsteps,"script/keyboard/gamepad gameplay diverged");
        boundaries(r3d,assets);visibility(r3d,assets);std::cout<<"shared device/script command routes, phase ordering, collection, reset and failure behavior passed\n";
    } catch(const std::exception& error){std::cerr<<error.what()<<'\n';const auto* required=std::getenv("KIN_REQUIRE_GPU_TESTS");return ready||(required&&std::string_view(required)!="0")?1:77;}
}
