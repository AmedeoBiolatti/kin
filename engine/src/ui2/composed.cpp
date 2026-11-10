#include <kin/ui2/composed.hpp>

#include <algorithm>
#include <cctype>

namespace kin::ui2 {
namespace {

constexpr std::string_view builtin_check =
    R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 16 16" fill="none" stroke="#000" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"><path d="M3.5 8.5l3 3 6-7"/></svg>)";
constexpr std::string_view builtin_chevron =
    R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 16 16" fill="none" stroke="#000" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"><path d="M6 3.5l4.5 4.5-4.5 4.5"/></svg>)";

// A ComposedLook with the theme's values where it gives none.
struct Look {
    TextStyle text;
    TextStyle faint;
    TextStyle bright;
    Color hover;
    Color lit;
    Color line;
    BoxStyle popup;
    f32 row_height = 26.0f;
    f32 corner = 4.0f;
    std::string_view check;
    std::string_view chevron;
};

Look resolve(const Context& ui, const ComposedLook& given) {
    const Theme& theme = ui.theme();
    Look look;
    look.text = given.text.value_or(theme.body_text);
    TextStyle faint = look.text;
    faint.color = theme.colors.text_muted;
    look.faint = given.faint.value_or(faint);
    TextStyle bright = look.text;
    bright.color = theme.colors.text_emphasis;
    look.bright = given.bright.value_or(bright);
    look.hover = given.hover.value_or(theme.colors.interactive_hovered);
    look.lit = given.lit.value_or(theme.colors.interactive_selected);
    look.line = given.line.value_or(theme.colors.border);
    if (given.popup) {
        look.popup = *given.popup;
    } else {
        look.popup = vbox();
        look.popup.pad(4).bg(theme.popup_surface.fill.a > 0 ? theme.popup_surface.fill : theme.colors.surface_panel)
            .border(theme.colors.border)
            .rounded(6.0f);
    }
    look.row_height = given.row_height;
    look.corner = given.corner;
    look.check = given.check_svg.empty() ? builtin_check : given.check_svg;
    look.chevron = given.chevron_svg.empty() ? builtin_chevron : given.chevron_svg;
    return look;
}

std::string lowered(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

// --- the menu bar's menus ----------------------------------------------------------

// Its entries as shown: submenus with entries to show; no line first, last,
// or after another.
std::vector<MenuEntry> shown_entries(const std::vector<MenuEntry>& entries) {
    std::vector<MenuEntry> out;
    for (const MenuEntry& entry : entries) {
        if (entry.line()) {
            if (!out.empty() && !out.back().line()) {
                out.push_back(entry);
            }
        } else if (entry.submenu()) {
            std::vector<MenuEntry> items = shown_entries(entry.items);
            if (!items.empty()) {
                out.push_back({.id = entry.id,
                               .label = entry.label,
                               .keys = entry.keys,
                               .enabled = entry.enabled,
                               .checked = entry.checked,
                               .items = std::move(items)});
            }
        } else {
            out.push_back(entry);
        }
    }
    while (!out.empty() && out.back().line()) {
        out.pop_back();
    }
    return out;
}

bool can_choose(const MenuEntry& entry) {
    if (entry.submenu()) {
        return std::ranges::any_of(entry.items, [](const MenuEntry& item) { return can_choose(item); });
    }
    return !entry.line() && entry.enabled;
}

// The next entry the keys can be on from `from`, going `way`, round the end;
// `from` when there is none.
i32 next_entry(const std::vector<MenuEntry>& entries, i32 from, i32 way) {
    const i32 count = static_cast<i32>(entries.size());
    if (count == 0) {
        return -1;
    }
    const i32 start = from < 0 ? (way > 0 ? -1 : count) : from;
    for (i32 step = 1; step <= count; ++step) {
        const i32 at = ((start + way * step) % count + count) % count;
        if (can_choose(entries[static_cast<std::size_t>(at)])) {
            return at;
        }
    }
    return from;
}

// A row of an open menu: a tick when on, its label, its keys or a chevron.
BoxResult entry_row(Context& ui, Id id, const MenuEntry& entry, bool lit, const Look& look) {
    const bool can = can_choose(entry);
    BoxStyle style = hbox().w(grow()).h(look.row_height).pad(6, 0, 10, 0).gap(6).items(UiAlign::Center).rounded(look.corner)
                         .selected_bg(look.lit).selected_if(lit && can);
    if (can) {
        style.hover_bg(look.lit);
    }
    auto row = ui.box(id, style);
    const SvgOptions mask{.mask = true};
    if (entry.checked) {
        ui.svg(look.check, {14, 14}, can ? look.bright.color : look.faint.color, mask);
    } else {
        ui.spacer({.width = fixed(14)});
    }
    TextStyle label = look.text;
    if (!can) {
        label.color = look.faint.color;
    }
    ui.label(entry.label, label, {.width = grow()}, TextOverflow::Ellipsis);
    if (entry.submenu()) {
        ui.svg(look.chevron, {12, 12}, look.faint.color, mask);
    } else if (!entry.keys.empty()) {
        ui.label(entry.keys, look.faint);
    }
    return row;
}

struct MenuBarState {
    i32 open = -1;       // the menu open (-1: none)
    i32 cursor = -1;     // the entry the keys are on
    i32 sub = -1;        // the entry whose submenu is open
    i32 sub_cursor = -1; // the submenu's entry the keys are on
    Vec2f pointer{};     // where the pointer was last frame: one that moved lights what it is over
};

// A press, Alt and the key's letter.
bool alt_letter(const Context& ui, char letter) {
    const char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(letter)));
    if (upper < 'A' || upper > 'Z') {
        return false;
    }
    const Key key = static_cast<Key>(static_cast<i32>(Key::A) + (upper - 'A'));
    return ui.modifier_held(KeyModifiers::Alt) && ui.key_pressed(key);
}

} // namespace

void close_menu_bar(Context& ui, Id id) {
    MenuBarState& state = ui.state<MenuBarState>(id);
    if (state.open >= 0) {
        ui.close_popup(make_id(id, "menu"));
    }
    state = {.pointer = state.pointer};
}

MenuBarResult menu_bar(Context& ui, Id id, std::span<const Menu> menus, const MenuBarOptions& options) {
    const Look look = resolve(ui, options.look);
    MenuBarState& state = ui.state<MenuBarState>(id);
    const Id popup = make_id(id, "menu");
    const Id sub_popup = make_id(id, "submenu");
    const Id titles_id = make_id(id, "titles");
    const i32 count = static_cast<i32>(menus.size());
    // Closed meanwhile (a press elsewhere, Esc, another popup), or its menu gone.
    if (state.open >= 0 && (!ui.popup_open(popup) || state.open >= count)) {
        state = {.pointer = state.pointer};
    }
    MenuBarResult result;
    // The pointer moved since last frame: what it is over takes over from the keys.
    const Vec2f pointer = ui.pointer();
    const bool moved = pointer.x != state.pointer.x || pointer.y != state.pointer.y;
    state.pointer = pointer;
    const auto open = [&](i32 at, bool by_keys) {
        state.open = at;
        state.cursor = by_keys ? next_entry(shown_entries(menus[static_cast<std::size_t>(at)].entries), -1, 1) : -1;
        state.sub = -1;
        state.sub_cursor = -1;
        ui.open_popup(popup);
    };
    for (i32 i = 0; i < count; ++i) {
        if (menus[static_cast<std::size_t>(i)].key != 0 && alt_letter(ui, menus[static_cast<std::size_t>(i)].key)) {
            open(i, true);
        }
    }

    // The titles.
    Rectf anchor{};
    {
        auto titles = ui.box(titles_id, hbox().gap(1).items(UiAlign::Center).passive());
        for (i32 i = 0; i < count; ++i) {
            const bool is_open = state.open == i;
            BoxStyle style = hbox().h(look.row_height).pad(8, 0).center().rounded(look.corner).hover_bg(look.hover);
            if (is_open) {
                style.bg(look.hover);
            }
            auto title = ui.box(make_id(titles_id, static_cast<u64>(i)), style);
            ui.label(menus[static_cast<std::size_t>(i)].title, is_open ? look.bright : look.text);
            if (title.pressed) {
                if (is_open) {
                    ui.close_popup(popup);
                    state = {.pointer = state.pointer};
                } else {
                    open(i, false);
                }
            } else if (state.open >= 0 && !is_open && title.hovered && moved) {
                open(i, false); // a menu open: another's title the pointer moves onto opens that one
            }
            if (state.open == i) {
                anchor = title.bounds;
            }
        }
    }
    if (state.open < 0) {
        return result;
    }

    // The keys: up and down it, into and out of a submenu, to the next menu; Enter chooses.
    std::optional<std::string_view> chosen;
    std::vector<MenuEntry> entries = shown_entries(menus[static_cast<std::size_t>(state.open)].entries);
    if (state.cursor >= static_cast<i32>(entries.size()) ||
        (state.sub >= 0 && (state.sub >= static_cast<i32>(entries.size()) || !entries[static_cast<std::size_t>(state.sub)].submenu()))) {
        state.cursor = -1;
        state.sub = -1;
        state.sub_cursor = -1;
    }
    const auto open_sub = [&](i32 at) {
        state.sub = at;
        state.sub_cursor = next_entry(entries[static_cast<std::size_t>(at)].items, -1, 1);
        ui.open_subpopup(popup, sub_popup);
    };
    if (!ui.typing()) {
        const bool in_sub = state.sub >= 0 && state.sub_cursor >= 0;
        const std::vector<MenuEntry> none;
        const std::vector<MenuEntry>& sub_entries = state.sub >= 0 ? entries[static_cast<std::size_t>(state.sub)].items : none;
        if (ui.key_typed(Key::Down) || ui.key_typed(Key::Up)) {
            const i32 way = ui.key_typed(Key::Down) ? 1 : -1;
            if (in_sub) {
                state.sub_cursor = next_entry(sub_entries, state.sub_cursor, way);
            } else {
                state.cursor = next_entry(entries, state.cursor, way);
                if (state.sub >= 0) {
                    ui.close_popup(sub_popup);
                    state.sub = -1;
                }
            }
        } else if (ui.key_pressed(Key::Right)) {
            if (!in_sub && state.cursor >= 0 && entries[static_cast<std::size_t>(state.cursor)].submenu()) {
                open_sub(state.cursor);
            } else if (count > 1) {
                open((state.open + 1) % count, true);
                entries = shown_entries(menus[static_cast<std::size_t>(state.open)].entries);
            }
        } else if (ui.key_pressed(Key::Left)) {
            if (state.sub >= 0) {
                ui.close_popup(sub_popup);
                state.sub = -1;
                state.sub_cursor = -1;
            } else if (count > 1) {
                open((state.open + count - 1) % count, true);
                entries = shown_entries(menus[static_cast<std::size_t>(state.open)].entries);
            }
        } else if (ui.key_pressed(Key::Enter) || ui.key_pressed(Key::KeypadEnter)) {
            if (in_sub && can_choose(sub_entries[static_cast<std::size_t>(state.sub_cursor)])) {
                chosen = sub_entries[static_cast<std::size_t>(state.sub_cursor)].id;
            } else if (state.cursor >= 0) {
                const MenuEntry& entry = entries[static_cast<std::size_t>(state.cursor)];
                if (entry.submenu()) {
                    open_sub(state.cursor);
                } else if (can_choose(entry)) {
                    chosen = entry.id;
                }
            }
        }
    }

    // The menu under its title, its submenu beside its entry (inside its box:
    // a press there is not outside it).
    BoxStyle box_style = look.popup;
    box_style.w(options.width);
    if (auto menu = ui.popup(popup, box_style, {.anchor = {anchor.x, anchor.y, anchor.w, anchor.h + 3.0f}, .opener = titles_id})) {
        Rectf sub_anchor{};
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const MenuEntry& entry = entries[i];
            const Id row_id = make_id(make_id(popup, "entry"), static_cast<u64>(i));
            const i32 at = static_cast<i32>(i);
            if (entry.line()) {
                auto rule = ui.box(row_id, hbox().w(grow()).h(1).bg(look.line).margins(6, 4).passive());
                continue;
            }
            const BoxResult row = entry_row(ui, row_id, entry, state.cursor == at || state.sub == at, look);
            if (row.hovered && moved) {
                state.cursor = at;
                if (entry.submenu()) {
                    if (state.sub != at) {
                        state.sub = at;
                        state.sub_cursor = -1;
                        ui.open_subpopup(popup, sub_popup);
                    }
                } else if (state.sub >= 0) {
                    ui.close_popup(sub_popup);
                    state.sub = -1;
                    state.sub_cursor = -1;
                }
            }
            if (row.clicked && can_choose(entry)) {
                if (entry.submenu()) {
                    state.sub = at;
                    ui.open_subpopup(popup, sub_popup);
                } else {
                    chosen = entry.id;
                }
            }
            if (state.sub == at) {
                sub_anchor = row.bounds;
            }
        }
        if (state.sub >= 0 && sub_anchor.w > 0.0f) {
            BoxStyle sub_style = look.popup;
            sub_style.w(options.submenu_width);
            if (auto sub = ui.popup(sub_popup, sub_style, {.anchor = sub_anchor, .side = UiFloat::Right, .offset = {8.0f, -4.0f}})) {
                const std::vector<MenuEntry>& items = entries[static_cast<std::size_t>(state.sub)].items;
                for (std::size_t i = 0; i < items.size(); ++i) {
                    const MenuEntry& entry = items[i];
                    const Id row_id = make_id(make_id(sub_popup, "entry"), static_cast<u64>(i));
                    if (entry.line()) {
                        auto rule = ui.box(row_id, hbox().w(grow()).h(1).bg(look.line).margins(6, 4).passive());
                        continue;
                    }
                    const BoxResult row = entry_row(ui, row_id, entry, state.sub_cursor == static_cast<i32>(i), look);
                    if (row.hovered && moved) {
                        state.sub_cursor = static_cast<i32>(i);
                    }
                    if (row.clicked && can_choose(entry)) {
                        chosen = entry.id;
                    }
                }
            } else {
                state.sub = -1; // closed (Esc, on top)
                state.sub_cursor = -1;
            }
        }
    } else {
        state = {.pointer = state.pointer}; // closed this frame (a press elsewhere, Esc)
    }
    if (chosen) {
        ui.close_popup(popup);
        state = {.pointer = state.pointer};
        result.chosen = chosen;
    }
    result.open = state.open >= 0;
    return result;
}

// --- a list to pick from -------------------------------------------------------------

namespace {

struct PickState {
    TextInput search{};
    i32 cursor = 0;
    bool open = false; // it was open last frame: the search is typed in from its first
};

} // namespace

std::optional<std::string> pick_list(Context& ui, Id id, std::span<const PickItem> items, Rectf anchor, const PickListOptions& options) {
    PickState& state = ui.state<PickState>(id);
    if (!ui.popup_open(id)) {
        state.open = false;
        return std::nullopt;
    }
    const Look look = resolve(ui, options.look);
    state.search.id = make_id(id, "search");
    if (!state.open) {
        state.search.state = {};
        state.search.state.active = true; // typed in at once
        state.cursor = 0;
        state.open = true;
    }
    // Those whose label or value has what was typed (any case).
    const std::string query = lowered(state.search.state.text);
    std::vector<const PickItem*> shown;
    for (const PickItem& item : items) {
        if (query.empty() || lowered(item.label).find(query) != std::string::npos || lowered(item.value).find(query) != std::string::npos) {
            shown.push_back(&item);
        }
    }
    const i32 count = static_cast<i32>(shown.size());
    if (ui.key_typed(Key::Down) && count > 0) {
        state.cursor = (state.cursor + 1) % count;
        ui.scroll_into_view(make_id(make_id(id, "item"), shown[static_cast<std::size_t>(state.cursor)]->value));
    } else if (ui.key_typed(Key::Up) && count > 0) {
        state.cursor = (state.cursor + count - 1) % count;
        ui.scroll_into_view(make_id(make_id(id, "item"), shown[static_cast<std::size_t>(state.cursor)]->value));
    }
    state.cursor = std::clamp(state.cursor, 0, std::max(0, count - 1));
    std::optional<std::string> picked;
    if (std::exchange(state.search.result.committed, false)) {
        if (count > 0) {
            picked = std::string(shown[static_cast<std::size_t>(state.cursor)]->value);
        } else if (options.free_text && !state.search.state.text.empty()) {
            picked = state.search.state.text; // one no item describes, by its name
        }
    }
    if (std::exchange(state.search.result.cancelled, false)) {
        ui.close_popup(id);
        state.open = false;
        return std::nullopt;
    }

    // Under the box it opens from, or over it when that is low in the window; its
    // right edge kept in the window.
    const Rectf root = ui.layout_root();
    const f32 list_height = std::min(options.max_height, look.row_height * static_cast<f32>(std::max(count, 1)) + 4.0f);
    const f32 width = std::max(anchor.w, options.width);
    const f32 height = list_height + look.row_height + 18.0f;
    const bool above = anchor.y > root.y + root.h * 0.55f;
    const Vec2f at{std::max(root.x + 8.0f, std::min(anchor.x, root.x + root.w - width - 8.0f)),
                   above ? anchor.y - height - 4.0f : anchor.y + anchor.h + 4.0f};
    BoxStyle box_style = look.popup;
    box_style.w(width).h(height).gap(4);
    if (auto box = ui.popup(id, box_style, {.point = at, .opener_bounds = anchor})) {
        {
            auto field = ui.box(make_id(id, "field"), hbox().w(grow()).passive());
            state.search.style = ui.theme().input;
            state.search.text_style = look.text;
            ui.widget(state.search, {.width = grow()});
            if (state.search.state.text.empty()) {
                const std::string_view hint = !options.placeholder.empty() ? options.placeholder
                                              : options.free_text           ? std::string_view("Search, or type a name")
                                                                            : std::string_view("Search");
                ui.label(hint, look.faint, LayoutStyle{}.at(UiAnchor::CenterLeft, {state.search.style.padding.left + 2.0f, 0.0f}));
            }
        }
        auto list = ui.scroll(make_id(id, "list"), vbox().w(grow()).h(list_height), {.scrollbar_thickness = 6.0f});
        if (shown.empty()) {
            ui.label(options.free_text && !state.search.state.text.empty() ? "Enter adds \"" + state.search.state.text + "\""
                                                                           : std::string("Nothing matches"),
                     look.faint, {.margin = {10, 8, 0, 0}});
        }
        for (std::size_t i = 0; i < shown.size(); ++i) {
            const PickItem& item = *shown[i];
            auto entry = ui.box(make_id(make_id(id, "item"), item.value),
                                hbox().w(grow()).h(look.row_height).pad(10, 0).gap(8).items(UiAlign::Center).rounded(look.corner)
                                    .hover_bg(look.lit).selected_bg(look.lit).selected_if(static_cast<i32>(i) == state.cursor)
                                    .cursor_shape(SystemCursor::Pointer));
            ui.label(item.label, look.text, {.width = grow()}, TextOverflow::Ellipsis);
            if (!item.note.empty()) {
                ui.label(item.note, look.faint, {}, TextOverflow::Ellipsis);
            }
            if (entry.clicked) {
                picked = std::string(item.value);
            }
        }
    } else {
        state.open = false; // closed this frame (a press elsewhere, Esc)
        return std::nullopt;
    }
    if (picked) {
        ui.close_popup(id);
        state.open = false;
    }
    return picked;
}

// --- breadcrumbs ---------------------------------------------------------------------

std::optional<std::size_t> breadcrumbs(Context& ui, Id id, std::span<const Crumb> crumbs, const ComposedLook& given) {
    const Look look = resolve(ui, given);
    std::optional<std::size_t> clicked;
    auto row = ui.box(id, hbox().w(grow()).h(look.row_height).gap(1).items(UiAlign::Center).clipped(true).passive());
    const SvgOptions mask{.mask = true};
    for (std::size_t i = 0; i < crumbs.size(); ++i) {
        const Crumb& part = crumbs[i];
        const bool last = i + 1 == crumbs.size();
        auto crumb = ui.box(make_id(id, static_cast<u64>(i)), hbox().h(look.row_height - 4.0f).pad(5, 0).gap(5).items(UiAlign::Center)
                                                                  .rounded(look.corner).hover_bg(look.hover).cursor_shape(SystemCursor::Pointer));
        if (!part.icon_svg.empty()) {
            ui.svg(part.icon_svg, {14, 14}, part.icon_tint, mask);
        }
        ui.label(part.label, last ? look.bright : look.faint, {}, TextOverflow::Ellipsis);
        if (crumb.clicked) {
            clicked = i;
        }
        if (!last) {
            ui.svg(look.chevron, {10, 10}, look.faint.color, mask);
        }
    }
    return clicked;
}

} // namespace kin::ui2
