#include "../examples/camp_walk/input.hpp"
#include <cassert>
#include <limits>
#include <iostream>

namespace {
using namespace camp_walk;
bool close(float a,float b){return std::abs(a-b)<.00001f;}
void mapping() {
    Input input;input.set_map(input_map());InputMapper mapper;constexpr float dt=1.f/60;
    input.set_key_pressed(Key::W);input.set_key_pressed(Key::D);input.set_key_pressed(Key::Space);input.set_key_pressed(Key::E);
    auto command=mapper.sample(input,{},dt).player;
    assert(close(command.move.x,.7071068f)&&close(command.move.y,.7071068f)&&command.jump&&command.interact);
    input.advance_step_edges();command=mapper.sample(input,{},dt).player;
    assert(command.move.y>0&&!command.jump&&!command.interact); // held is not another edge
    InputMap rebound=input_map();rebound.unbind("camp.interact");rebound.bind("camp.interact",Key::Q);
    input.set_map(rebound);input.set_key_pressed(Key::Q);assert(mapper.sample(input,{},dt).player.interact);
    InputActionContext hints;collect_actions(hints);const auto actions=hints.resolve(rebound);
    assert(actions.size()==14);for(const auto& action:actions)assert(!action.bindings.empty());
    Input mouse;mouse.set_map(input_map());mouse.set_mouse_motion({10,-5});
    assert(mapper.sample(mouse,{},dt).player.look.x==0);
    mouse.set_mouse_held(MouseButton::Right,true);command=mapper.sample(mouse,{},dt).player;
    assert(close(command.look.x,1.2f)&&close(command.look.y,.6f));
    mouse.advance_step_edges();assert(mapper.sample(mouse,{},dt).player.look.x==0);
}
void pads() {
    Input input;input.set_map(input_map());InputMapper mapper;GamepadState pad;pad.connected=true;
    pad.left_x=.1f;pad.right_y=.1f;assert(mapper.sample(input,pad,1.f/60).player.move.x==0);
    pad.left_x=1;pad.right_y=1;pad.set_button(GamepadButton::A,true);pad.set_button(GamepadButton::X,true);
    auto command=mapper.sample(input,pad,1.f/60).player;
    assert(command.move.x==1&&command.jump&&command.interact&&close(command.look.y,2));
    command=mapper.sample(input,pad,1.f/60).player;assert(!command.jump&&!command.interact);
    pad.connected=false;command=mapper.sample(input,pad,1.f/60).player;assert(command.move.x==0&&command.look.y==0&&!command.jump);
    pad.connected=true;assert(mapper.sample(input,pad,1.f/60).player.jump);
    mapper.settings.interact=GamepadButton::RightShoulder;pad.set_button(GamepadButton::RightShoulder,true);
    assert(mapper.sample(input,pad,1.f/60).player.interact);
    pad.left_x=std::numeric_limits<float>::quiet_NaN();assert(mapper.sample(input,pad,1.f/60).player.move.x==0);
    assert(close(stick({.59f,0},.18f).x,.5f)); // rescaled analog magnitude
    Input keys;keys.set_map(input_map());keys.set_key_pressed(Key::D);keys.set_key_pressed(Key::Space);
    InputMapper keyboard,pads;GamepadState digital;digital.connected=true;digital.left_x=1;digital.set_button(GamepadButton::A,true);
    const auto a=keyboard.sample(keys,{},1.f/60).player,b=pads.sample(input,digital,1.f/60).player;
    assert(a.move.x==b.move.x&&a.move.y==b.move.y&&a.jump==b.jump);
}
}
int main(){mapping();pads();std::cout<<"named actions, step edges, rebinding, mouse and gamepad commands passed\n";}
