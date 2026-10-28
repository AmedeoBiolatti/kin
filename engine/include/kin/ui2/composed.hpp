#pragma once

// Widgets made of boxes (the composition API: Context::box, popup), as a
// desktop editor wants them: a menu bar, a list to pick from with a search,
// a path's breadcrumbs. Their state is the Context's (Context::state, its
// popup slots); what they show is given each frame.

#include <kin/ui2/context.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kin::ui2 {

// How they look: the theme's (its body text, muted text, borders, popup
// surface) unless given.
struct ComposedLook {
    std::optional<TextStyle> text{};   // a title's, an entry's
    std::optional<TextStyle> faint{};  // keys, notes, what is not the last crumb
    std::optional<TextStyle> bright{}; // an open title's, the last crumb's
    std::optional<Color> hover{};      // a row under the pointer
    std::optional<Color> lit{};        // the row the keys are on
    std::optional<Color> line{};       // a menu's lines between its groups
    std::optional<BoxStyle> popup{};   // a menu's box (its fill, border, corners, shadow, padding)
    f32 row_height = 26.0f;
    f32 corner = 4.0f;
    std::string_view check_svg{};   // an entry that is on; ui2's own when empty
    std::string_view chevron_svg{}; // a submenu's, between crumbs; ui2's own when empty
};

// --- a menu bar ---------------------------------------------------------------------

// An entry of a menu: views, read while the bar is built.
struct MenuEntry {
    std::string_view id{};          // what choosing it reports
    std::string_view label{};
    std::string_view keys{};        // at its right: "Ctrl+S"
    bool enabled = true;            // greyed and not chosen when false
    bool checked = false;           // a tick at its left: a tool in use, a panel shown
    std::vector<MenuEntry> items{}; // a submenu's entries
    bool line() const { return id.empty() && label.empty() && items.empty(); } // a line between groups
    bool submenu() const { return !items.empty(); }
};
struct Menu {
    std::string_view id{};
    std::string_view title{};
    char key = 0; // Alt and this letter open it ('F' for File)
    std::vector<MenuEntry> entries{};
};
struct MenuBarOptions {
    ComposedLook look{};
    f32 width = 280.0f;         // a menu's
    f32 submenu_width = 220.0f; // a submenu's
};
struct MenuBarResult {
    std::optional<std::string_view> chosen; // an entry's id, the frame it is chosen
    bool open = false;                      // one of its menus is open (the keys are its)
};
// The titles of `menus` (in the row the caller builds), and the one open
// under its title: opened by a press on it, by Alt and its letter, by the
// pointer moving to its title while another is open; walked with the arrows
// (Right into a submenu, else to the next menu), chosen with Enter or a click;
// closed by Esc, a press elsewhere, or another popup opening. Lines that would
// start or end a menu, or follow another, are not shown; nor are empty submenus.
MenuBarResult menu_bar(Context& ui, Id id, std::span<const Menu> menus, const MenuBarOptions& options = {});
void close_menu_bar(Context& ui, Id id); // another menu of the app's opens

// --- a list to pick from --------------------------------------------------------------

struct PickItem {
    std::string_view value{}; // what picking it returns
    std::string_view label{};
    std::string_view note{};  // after it, faint
};
struct PickListOptions {
    bool free_text = false;         // Enter with nothing matching picks what was typed
    std::string_view placeholder{}; // in the search; "Search" when empty
    f32 width = 240.0f;             // at least the anchor's
    f32 max_height = 260.0f;        // its list's; it scrolls past it
    ComposedLook look{};
};
// A popup (open_popup(id)) under `anchor` (over it when that is low in the
// window): a search field, typed in at once, over the items matching it (a
// label's or value's letters, together, any case); Up and Down walk them,
// Enter or a click picks one, Esc or a press elsewhere closes it. What was
// picked, the frame it is: the popup closes then.
std::optional<std::string> pick_list(Context& ui, Id id, std::span<const PickItem> items, Rectf anchor, const PickListOptions& options = {});

// --- breadcrumbs --------------------------------------------------------------------

struct Crumb {
    std::string_view label{};
    std::string_view icon_svg{}; // before its label (a file's kind)
    Color icon_tint = colors::white;
};
// A path as crumbs with a chevron between them, the last brighter; which was
// clicked.
std::optional<std::size_t> breadcrumbs(Context& ui, Id id, std::span<const Crumb> crumbs, const ComposedLook& look = {});

} // namespace kin::ui2
