#pragma once

// A Lua script on an entity: a file of the game's content (`asset`, a .lua)
// that EntityScripts (kin/scripting/entity_scripts.hpp) runs for it. Each
// entity running a script has a `self` of its own; the script's functions
// are called with it:
//
//     -- scripts/player.lua
//     function on_start(self)
//         self.speed = 160
//     end
//
//     function on_update(self, dt)
//         self:move(input.axis("move_x") * self.speed * dt, 0)
//         if input.pressed("jump") then self:trigger("jump") end
//     end
//
//     function on_contact(self, other)
//         if other:has("Coin") then other:destroy() end
//     end
//
// In a game for several players at one screen, `player` says whose input its
// script's input.* reads: the same player.lua runs player 1's character and
// player 2's.

#include <kin/ecs/component.hpp>

namespace kin {

struct ScriptComponent {
    ComponentAssetRef asset; // a .lua file of the content
    bool enabled = true;     // false: not run (a disabled entity's are not either)
    i32 player = 0;          // whose input it reads: 0 anyone's, 1 the first player's, 2 the second's...
};

} // namespace kin
