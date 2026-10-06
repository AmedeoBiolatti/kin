#pragma once

// Translations for kin's Lua hosts (ScriptEngine, LuaScript), read from the
// active localization (kin/l10n/localization.hpp):
//
//   tr("menu.play")                       the text for a key
//   tr("shop.gold", {gold = 1250})        with values put in
//   l10n.locale()                         "fr" ("" without a localization)
//   l10n.direction()                      "ltr" or "rtl"
//   l10n.has("menu.play")
//   l10n.format("{n} left", {n = 3})      a pattern of the script's own
//   l10n.languages()                      {{locale=, name=, direction=}, ...}
//   l10n.set_locale("de")                 the locale now shown (when allowed)

#include <sol/forward.hpp>

namespace kin::scripting_detail {

void bind_l10n(sol::state_view lua, bool allow_switching);

} // namespace kin::scripting_detail
