#pragma once

// Painting the HUD and the menu with ui2 (docs/3d_plan.md, P4b). What is drawn and where is decided in hud.hpp (the
// model, the layout, the menu): these only turn it into panels and text, so there is nothing in here to test without a
// window.

#include "hud.hpp"

#include <kin/ui2/context.hpp>

namespace summit {

// The HUD over the 3D picture. Call between ui.begin() and ui.end(). `time` is the presentation clock (the pulse of a
// warning); `debug` is F3's line (empty: not shown).
void draw_hud(ui2::Context& ui, const HudModel& model, const HudLayout& layout, const InputBindings& bindings, f64 time,
              const std::string& debug);

// The pause menu or the controls screen over everything; `pointer` highlights the row under the mouse.
void draw_menu(ui2::Context& ui, const Menu& menu, const MenuLayout& layout, const InputBindings& bindings,
               Vec2f screen, Vec2f pointer);

} // namespace summit
