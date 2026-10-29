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

#include <kin/ecs/component.hpp>

namespace kin {

struct ScriptComponent {
    ComponentAssetRef asset; // a .lua file of the content
    bool enabled = true;     // false: not run (a disabled entity's are not either)
};

} // namespace kin
