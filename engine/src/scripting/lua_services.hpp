#pragma once

// The platform's services for kin's Lua hosts (ScriptEngine, LuaScript), from
// the active ones (kin/platform/services.hpp); without any, every call does
// nothing and returns false or nil.
//
//   services.backend()                          "steam", "offline", ...
//   services.online()
//   services.player()                           {id=, name=}
//   services.language()  services.handheld()  services.big_picture()
//   services.unlock("FIRST_WIN")                services.unlocked(id)  services.clear_achievement(id)
//   services.progress("CHESTS", 3, 10)
//   services.set_stat("kills", 12)              whole numbers as Lua integers, else decimal
//   services.add_stat("kills", 1)               services.stat("kills")
//   services.upload_score("arena", 1200, {details}, {keep="best"|"replace", sort="desc"|"asc",
//                         display="number"|"seconds"|"milliseconds"})   -> request number
//   services.request_scores("arena", "top"|"around"|"friends", first, last) -> request number
//   services.presence("status", "In the vale")  services.clear_presence()  services.join_command("lobby:1")
//   services.overlay_active()                   services.open_overlay("achievements")
//   services.open_url(url)  services.open_store([dlc])  services.invite(lobby)
//   services.ask_text(prompt, [initial], [max], [password])
//   services.owns(dlc)  services.join_on_launch()  services.flush()
//   for _, e in ipairs(services.events()) do  -- this frame's: e.kind ("overlay_shown", "join_requested", ...),
//       ...                                   -- e.id, e.text, e.request, e.ok, e.changed, e.rank, e.entries
//   end

#include <sol/forward.hpp>

namespace kin::scripting_detail {

void bind_services(sol::state_view lua);

} // namespace kin::scripting_detail
