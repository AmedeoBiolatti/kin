#pragma once

// Ordinary game code using the existing scene, command-buffer, controller and
// system APIs. No device reads, editor schema, alternate world or task scheduler.
#include "input.hpp"
#include <kin/scene/camera_rig3d.hpp>
#include <kin/scene/interaction3d.hpp>
#include <kin/ecs/system.hpp>
#include <kin/anim/events.hpp>
#include <array>
#include <stdexcept>

namespace camp_walk {
enum : InteractionVerb3D { Collect=1,Push=2 };
inline std::string_view interaction_prompt(InteractionVerb3D verb,bool gamepad=false) {
    switch(verb){case Collect:return gamepad?"X collect":"E collect";case Push:return gamepad?"Y push":"F push";default:return {};}
}
struct Supply { int amount=1;float phase=0; };
struct Progress {
    int steps=0,grounded=0,airborne=0,input_steps=0,movement_steps=0,interaction_steps=0;
    int camera_steps=0;
    int footsteps=0,animation_events_dropped=0,animation_events_unhandled=0;
    int pickups_started=0,pickup_contacts=0,pickups_cancelled=0,pickup_awards=0;
    int collected_total=0,rounds_completed=0,level_resets=0,spawned_total=0,pushes=0;
    bool jumped=false,crouched=false,moved=false,stale_handles_rejected=false,crate_moved=false;
};
inline CameraRig3DSettings camera_settings() {
    CameraRig3DSettings settings;settings.target_offset={0,.9f,0};settings.arm.distance=8;settings.arm.shoulder=0;
    settings.pitch_min=-65;settings.pitch_max=-5;settings.initial_pitch=-15;return settings;
}
struct State {
    Progress progress;
    std::array<ScenePrefab3D,3> supplies{};
    std::array<SceneRequest3D,3> removals{};
    std::array<int,3> awards{};
    ScenePrefab3D crate,lantern;
    Vec3f crate_start{};
    int collected=0,nearby=-1;
    CameraRig3D camera{camera_settings()};
    std::optional<CameraRig3DFrame> presentation;
    bool prepared=false,restart_requested=false,query_overflow=false;
    int query_overflows=0;
    std::array<Vec3f,AnimationEventBatch3D::capacity> footstep_positions{};
    u32 footstep_count=0;
    InteractionQuery3D collect_query{{.reach=1.25f,.candidates=layer_bit(layers3d::Sensor),
        .candidate_sensors=true,.sight_sensors=true}};
    InteractionQuery3D push_query{{.reach=1.8f}};
    InteractionFocus3D pickup,push_focus;
    ActionToken3D pickup_token;
    int pickup_index=-1;
    bool pickup_contact=false;
};
struct CommandState { PlayerCommand pending{},current{}; };
inline const State& state(const kin::EcsWorld& world) {return *world.raw().get<State>();}
// Submit a fresh command each step. If no command arrives, the next step is
// neutral: held movement and one-shot actions cannot accidentally repeat.
inline void submit(kin::EcsWorld& world,PlayerCommand command) {
    command.move=unit_move(command.move);
    if(!std::isfinite(command.look.x)||!std::isfinite(command.look.y))command.look={};
    world.raw().get_mut<CommandState>()->pending=command;
}
inline void register_components(kin::EcsWorld& world) {
    world.component<Supply>("CampSupply");
    world.systems().register_native<Supply,Transform3D>({.id="camp.supply.spin",.phase=SystemPhase::Animation,
        .run=[](kin::EcsEntity,Supply& supply,Transform3D& transform,SystemContext& context) {
            supply.phase=std::fmod(supply.phase+context.dt*45,360.f);transform.rotation=Quatf::axis_angle({0,1,0},supply.phase);
        }});
}
inline void cancel_pickup(State& game,kin::EcsEntity player) {
    if(game.pickup.target) {
        cancel_action3d(player,game.pickup_token);
        ++game.progress.pickups_cancelled;
    }
    game.pickup={};game.pickup_token={};game.pickup_index=-1;game.pickup_contact=false;
}
inline InteractionObserver3D observer(const SceneQueries3D& queries,kin::EcsEntity player,kin::EcsEntity camera) {
    return {queries.handle(player),player.get<CharacterController3D>()->state.position+Vec3f{0,.3f,0},
        camera.get<Transform3D>()->position};
}
inline std::optional<InteractionOffer3D> supply_offer(const State& game,const SceneQueries3D& queries,kin::EcsEntity entity) {
    if(!entity.has<Supply>()||!entity.has<WorldTransform3D>())return {};
    for(size_t i=0;i<game.supplies.size();++i)
        if(game.supplies[i].root==queries.handle(entity)&&game.removals[i].status()==SceneRequestStatus3D::Invalid)
            return InteractionOffer3D{entity.get<WorldTransform3D>()->matrix.apply({})+Vec3f{0,.3f,0},Collect};
    return {};
}
inline void install(SceneSession3DContext& scene) {
    auto player=scene.instance.by_authored_id.at("player"),camera=scene.instance.by_authored_id.at("camera");
    if(!scene.physics||!scene.bodies||!player.has<CharacterController3D>())throw std::runtime_error("camp needs player physics");
    scene.world.raw().set(State{});scene.world.raw().set(CommandState{});
    scene.world.systems().register_task({.id="camp.input",.phase=SystemPhase::Input},[](SystemContext& context) {
        auto& world=*context.world;auto* input=world.raw().get_mut<CommandState>();
        input->current=input->pending;input->pending={};++world.raw().get_mut<State>()->progress.input_steps;
    });
    scene.world.systems().register_task({.id="camp.movement",.phase=SystemPhase::PrePhysics},[player](SystemContext& context) mutable {
        auto& world=*context.world;auto* game=world.raw().get_mut<State>();
        if(!game->prepared)throw std::runtime_error("camp population must be prepared before stepping");
        const auto command=world.raw().get<CommandState>()->current;
        game->camera.orbit(command.look);
        const auto yaw=Quatf::axis_angle({0,1,0},game->camera.yaw());
        const Vec3f forward=yaw.rotate({0,0,-1}),right=yaw.rotate({1,0,0});
        auto* controller=player.get_mut<CharacterController3D>();
        controller->input={.move=command.move,.forward=forward,.jump=command.jump,.crouch=command.crouch,.sprint=command.sprint};
        if(command.move.x*command.move.x+command.move.y*command.move.y>.0001f)
            controller->facing=Quatf::look_rotation(-normalize(right*command.move.x+forward*command.move.y));
        ++game->progress.movement_steps;
    });
    scene.world.systems().register_task({.id="camp.camera",.phase=SystemPhase::PostPhysics,.order=-1,.before={"camp.interaction"}},
        [camera,queries=scene.queries](SystemContext& context) mutable {
            auto* game=context.world->raw().get_mut<State>();
            if(!game->camera.observe(*context.world))throw std::runtime_error("camp camera target missing");
            const auto frame=game->camera.fixed_frame(queries,context.dt);
            if(!frame)throw std::runtime_error("camp camera query unavailable");
            auto* transform=camera.get_mut<Transform3D>();transform->position=frame->camera.position;transform->rotation=frame->camera.rotation;
            ++game->progress.camera_steps;
        });
    scene.world.systems().register_task({.id="camp.interaction",.phase=SystemPhase::PostPhysics,
        .structural_mutation_policy=SystemStructuralMutationPolicy::CommandBufferOnly},
        [player,camera,physics=scene.physics,bodies=scene.bodies,commands=scene.commands,
         queries=scene.queries](SystemContext& context) mutable {
        auto& world=*context.world;auto* game=world.raw().get_mut<State>();auto& progress=game->progress;
        const auto& player_state=player.get<CharacterController3D>()->state;
        ++progress.steps;++progress.interaction_steps;
        if(player_state.ground==CharacterGround3D::OnGround)++progress.grounded;else++progress.airborne;
        progress.jumped|=player_state.jumped;progress.crouched|=player_state.crouching;progress.moved|=std::abs(player_state.position.x-1)>.5f;
        game->restart_requested=player_state.position.y < -4;
        const auto command=world.raw().get<CommandState>()->current;
        if(game->pickup.target) {
            const auto status=action_status3d(player,game->pickup_token);
            if(player_state.ground!=CharacterGround3D::OnGround || status==ActionStatus3D::Cancelled ||
               status==ActionStatus3D::Superseded || status==ActionStatus3D::Completed || status==ActionStatus3D::None)
                cancel_pickup(*game,player);
        }
        game->nearby=-1;
        std::array<SceneOverlapHit3D,8> candidates;
        const auto selected=game->collect_query.select(queries,observer(queries,player,camera),candidates,
            [&](kin::EcsEntity entity){return supply_offer(*game,queries,entity);});
        if(selected.status==InteractionStatus3D::Unavailable||selected.status==InteractionStatus3D::InvalidObserver)
            throw std::runtime_error("camp scene queries unavailable");
        game->query_overflow=selected.status==InteractionStatus3D::Overflow;
        if(game->query_overflow)++game->query_overflows;
        if(selected)for(size_t i=0;i<game->supplies.size();++i)
            if(game->supplies[i].root==selected.focus.target){game->nearby=int(i);break;}
        for(size_t i=0;i<game->supplies.size();++i)if(auto entity=game->supplies[i].root.resolve(world)) {
            const bool selected=int(i)==game->nearby;
            entity.get_mut<ModelRenderer3D>()->tint=selected?Color::rgb(255,230,100):colors::white;
            entity.get_mut<PointLight3D>()->color=selected?Vec3f{1.6f,1,.1f}:Vec3f{.15f,1.2f,.4f};
        }
        if(command.interact&&game->nearby>=0&&!game->pickup.target&&player_state.ground==CharacterGround3D::OnGround) {
            const auto token=play_action3d(player,"pickup");
            if(token) {
                game->pickup_index=game->nearby;game->pickup=selected.focus;
                game->pickup_token=token;++progress.pickups_started;game->nearby=-1;
            }
        }
        game->push_focus={};
        if(auto crate=game->crate.root.resolve(world)) {
            const auto body=bodies->body_of(crate.raw());const auto position=physics->position(body);
            auto moved=position-game->crate_start;moved.y=0;progress.crate_moved|=length_squared(moved)>.08f*.08f;
            auto direction=position-player_state.position;direction.y=0;
            // A direct known target uses the same range/sight validation as
            // discovered pickups. This verb has no camera-visibility requirement.
            const InteractionFocus3D locked{game->crate.root,Push};
            const auto push=game->push_query.revalidate(queries,
                {queries.handle(player),player_state.position+Vec3f{0,.3f,0}},locked,
                [&](kin::EcsEntity)->std::optional<InteractionOffer3D>{return InteractionOffer3D{position+Vec3f{0,.4f,0},Push};});
            if(push)game->push_focus=push.focus;
            if(command.push&&push) {
                physics->add_impulse(body,length_squared(direction)>.001f?normalize(direction)*6:Vec3f{6,0,0});++progress.pushes;
            }
        }
    });
    // Audio follows the stock Animation boundary even when no frames render.
    // Reuse normal event handlers; output expires each step instead of queuing.
    auto dispatch=std::make_shared<AnimationEventDispatch>();
    auto dust=scene.instance.by_authored_id.at("foot-dust");
    dispatch->on("camp",[dust](kin::EcsEntity actor,const AnimationEvent& event) mutable {
        auto* game=actor.raw().world().get_mut<State>();
        if(event.name=="pickup_contact") {
            if(game->pickup.target&&game->pickup_token==actor.get<Animator3D>()->player.current_action())game->pickup_contact=true;
            return;
        }
        if(event.name!="footstep")return;
        const auto* controller=actor.get<CharacterController3D>();
        if(!controller||controller->state.ground!=CharacterGround3D::OnGround)return;
        auto velocity=controller->state.velocity;velocity.y=0;
        if(length_squared(velocity)<.04f)return;
        const float side=event.value=="left"?-.12f:.12f;
        const auto position=controller->state.position+controller->facing.rotate({side,.04f,0});
        dust.get_mut<ParticleEmitter3D>()->system.burst_at(position,3);
        if(game->footstep_count<game->footstep_positions.size())game->footstep_positions[game->footstep_count++]=position;
        ++game->progress.footsteps;
    });
    scene.world.systems().register_task({.id="camp.animation.events",.phase=SystemPhase::Audio,
        .structural_mutation_policy=SystemStructuralMutationPolicy::CommandBufferOnly},
        [player,camera,dispatch,queries=scene.queries,commands=scene.commands](SystemContext& context) mutable {
        auto* game=context.world->raw().get_mut<State>();game->footstep_count=0;game->pickup_contact=false;
        const auto dropped=player.get<Animator3D>()->events.dropped;
        game->progress.animation_events_dropped+=int(dropped);
        game->progress.animation_events_unhandled+=int(dispatch_animation_events3d(*context.world,*dispatch));
        if(dropped&&game->pickup.target){cancel_pickup(*game,player);return;}
        if(!game->pickup_contact||!game->pickup.target)return;
        ++game->progress.pickup_contacts;
        const auto index=size_t(game->pickup_index);
        if(index>=game->supplies.size()||game->query_overflow||game->supplies[index].root!=game->pickup.target||
           player.get<CharacterController3D>()->state.ground!=CharacterGround3D::OnGround||
           !game->collect_query.revalidate(queries,observer(queries,player,camera),game->pickup,
               [&](kin::EcsEntity entity){return supply_offer(*game,queries,entity);})) {cancel_pickup(*game,player);return;}
        const auto target=queries.resolve(game->pickup.target);
        game->awards[index]=target.get<Supply>()->amount;
        game->removals[index]=commands.despawn(*context.commands,game->supplies[index]);
        // The removal commits at the late boundary. Award next Animation phase
        // only after its checked operation has succeeded.
        game->pickup={};game->pickup_token={};game->pickup_index=-1;game->pickup_contact=false;
    });
    // The previous late boundary committed action removals. Award only successful
    // operations, once; failed/cancelled requests are visible errors, not points.
    scene.world.systems().register_task({.id="camp.collection",.phase=SystemPhase::Animation,.order=-1,.before={"camp.supply.spin"}},[](SystemContext& context) {
        auto* game=context.world->raw().get_mut<State>();
        for(size_t i=0;i<game->removals.size();++i) {
            const auto& removal=game->removals[i];
            if(removal.status()==SceneRequestStatus3D::Failed||removal.status()==SceneRequestStatus3D::Cancelled)throw std::runtime_error(removal.error());
            if(removal.status()!=SceneRequestStatus3D::Succeeded)continue;
            const int previous=game->collected;
            ++game->progress.pickup_awards;
            game->collected+=game->awards[i];game->progress.collected_total+=game->awards[i];
            game->supplies[i]={};game->removals[i]={};game->awards[i]=0;
            if(previous<3&&game->collected>=3)++game->progress.rounds_completed;
        }
    });
}
// Explicit level-loading operations, between steps. Persistent progress is a
// game policy carried only after successful scene replacement, never by a
// candidate's initialization callback. Five runtime prefabs, eight entities maximum.
inline bool prepare(SceneSession3D& session,Progress progress={}) {
    auto* game=session.world()->raw().get_mut<State>();if(game->prepared)return false;
    game->progress=progress;
    constexpr std::array<Vec3f,3> positions{{{1.6f,.05f,1.4f},{-.8f,.05f,.8f},{.8f,.05f,.2f}}};
    for(size_t i=0;i<game->supplies.size();++i) {
        game->supplies[i]=session.spawn("prefabs/camp-supply.kinprefab",{.position=positions[i],
            .configure=[i](SceneSpawnContext3D& created){created.root.set(Supply{1,float(i)*120});}});
        if(!game->supplies[i])return false;
    }
    game->crate=session.spawn("prefabs/camp-crate.kinprefab",{.position=Vec3f{1.15f,.12f,-.5f}});if(!game->crate)return false;
    game->crate_start=session.physics()->position(session.bodies()->body_of(session.resolve(game->crate.root).raw()));
    const auto player=session.handle(session.entity("player"));
    game->lantern=session.spawn("prefabs/camp-lantern.kinprefab",{.configure=[player](SceneSpawnContext3D& created) {
        created.root.set(JointAttachment3D{player,"hand_R"});
    }});
    if(!game->lantern)return false;
    game->camera.follow(player);
    session.entity("player").set(RenderTransform3D{});
    if(!game->camera.observe(*session.world()))return false;
    game->progress.spawned_total+=5;game->prepared=true;return true;
}
// Presentation is called by the host, once per rendered frame. The override
// affects only this entity's drawing, never its hierarchy or controller pose.
inline bool present(SceneSession3D& session,float alpha,float seconds) {
    auto* game=session.world()->raw().get_mut<State>();
    game->presentation=game->camera.frame(session.queries(),alpha,seconds);
    auto player=session.entity("player");auto* placement=player.get_mut<RenderTransform3D>();
    placement->enabled=bool(game->presentation);
    if(game->presentation)placement->matrix=game->presentation->target_transform;
    return bool(game->presentation);
}
inline void compose(SceneSession3DContext& context,Scene3D& scene) {
    const auto& game=state(context.world);if(game.presentation)scene.camera=game.presentation->camera;
}
inline bool restart(SceneSession3D& session,bool refresh=false) {
    auto progress=state(*session.world()).progress;
    if(state(*session.world()).pickup.target)++progress.pickups_cancelled;
    const auto old=state(*session.world()).crate.root;
    if(!session.reload(refresh))return false;
    ++progress.level_resets;progress.stale_handles_rejected|=bool(old)&&!session.resolve(old);
    return prepare(session,progress);
}
inline bool demo_passed(const State& game) {
    const auto& p=game.progress;
    return p.jumped&&p.crouched&&p.moved&&p.grounded>=60&&game.collected==3&&p.crate_moved&&p.pushes&&p.stale_handles_rejected;
}
} // namespace camp_walk
