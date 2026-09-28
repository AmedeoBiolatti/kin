#include <kin/ecs/ui2_sync.hpp>

#include "ui2_internal.hpp"

#include <algorithm>
#include <string>
#include <type_traits>

namespace kin {
namespace {

template <typename T, typename U>
void remove_component_if_not(EcsEntity entity) {
    if constexpr (!std::is_same_v<T, U>) {
        if (entity.has<U>()) {
            entity.remove<U>();
        }
    }
}

template <typename T>
void remove_other_v1_widgets(EcsEntity entity) {
    remove_component_if_not<T, ui2::Panel>(entity);
    remove_component_if_not<T, ui2::Label>(entity);
    remove_component_if_not<T, ui2::WrappedText>(entity);
    remove_component_if_not<T, ui2::Separator>(entity);
    remove_component_if_not<T, ui2::Button>(entity);
    remove_component_if_not<T, ui2::Toggle>(entity);
    remove_component_if_not<T, ui2::Slider>(entity);
    remove_component_if_not<T, ui2::ProgressBar>(entity);
    remove_component_if_not<T, ui2::Image>(entity);
    remove_component_if_not<T, ui2::Spacer>(entity);
    remove_component_if_not<T, ui2::IconButton>(entity);
    remove_component_if_not<T, ui2::Meter>(entity);
    remove_component_if_not<T, ui2::TextInput>(entity);
    remove_component_if_not<T, ui2::ListView>(entity);
    remove_component_if_not<T, ui2::TabBar>(entity);
    remove_component_if_not<T, ui2::MenuList>(entity);
}

bool same_items(const std::vector<std::string>& lhs, const std::vector<std::string>& rhs) {
    return lhs == rhs;
}

bool same_menu_items(const std::vector<ui2::MenuListItem>& lhs, const std::vector<ui2::MenuListItem>& rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        if (lhs[i].id != rhs[i].id ||
            lhs[i].label != rhs[i].label ||
            lhs[i].submenu_id != rhs[i].submenu_id ||
            lhs[i].enabled != rhs[i].enabled ||
            lhs[i].checked != rhs[i].checked ||
            lhs[i].separator != rhs[i].separator ||
            lhs[i].danger != rhs[i].danger ||
            lhs[i].default_item != rhs[i].default_item ||
            lhs[i].prompt != rhs[i].prompt ||
            lhs[i].tooltip != rhs[i].tooltip) {
            return false;
        }
    }
    return true;
}

bool same_tab_items(const std::vector<ui2::TabBarItem>& lhs, const std::vector<ui2::TabBarItem>& rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        if (lhs[i].id != rhs[i].id ||
            lhs[i].label != rhs[i].label ||
            lhs[i].dirty != rhs[i].dirty ||
            lhs[i].closable != rhs[i].closable ||
            lhs[i].enabled != rhs[i].enabled) {
            return false;
        }
    }
    return true;
}

// Text measurement depends on the font backend and scale, so a theme/font swap
// must re-measure even when the string is unchanged.
bool text_measure_equal(const ui2::TextStyle& lhs, const ui2::TextStyle& rhs) {
    return lhs.font.identity() == rhs.font.identity() && lhs.scale == rhs.scale;
}

bool measure_dirty(const ui2::Panel&, const ui2::Panel&) { return false; }
bool measure_dirty(const ui2::Label& old, const ui2::Label& next) {
    return old.text != next.text || old.horizontal != next.horizontal || old.vertical != next.vertical ||
           !text_measure_equal(old.text_style, next.text_style);
}
bool measure_dirty(const ui2::WrappedText& old, const ui2::WrappedText& next) {
    return old.text != next.text || old.line_spacing != next.line_spacing ||
           !text_measure_equal(old.text_style, next.text_style);
}
bool measure_dirty(const ui2::Separator& old, const ui2::Separator& next) { return old.axis != next.axis || old.thickness != next.thickness; }
bool measure_dirty(const ui2::Button& old, const ui2::Button& next) {
    return old.label != next.label || !text_measure_equal(old.text_style, next.text_style);
}
bool measure_dirty(const ui2::Toggle& old, const ui2::Toggle& next) {
    return old.label != next.label || !text_measure_equal(old.text_style, next.text_style);
}
bool measure_dirty(const ui2::Slider&, const ui2::Slider&) { return false; }
bool measure_dirty(const ui2::ProgressBar&, const ui2::ProgressBar&) { return false; }
bool measure_dirty(const ui2::Image& old, const ui2::Image& next) { return old.size != next.size || old.fit != next.fit; }
bool measure_dirty(const ui2::Spacer& old, const ui2::Spacer& next) { return old.size != next.size; }
bool measure_dirty(const ui2::IconButton& old, const ui2::IconButton& next) {
    return old.label != next.label || old.icon_size != next.icon_size || old.label_placement != next.label_placement ||
           !text_measure_equal(old.text_style, next.text_style);
}
bool measure_dirty(const ui2::Meter&, const ui2::Meter&) { return false; }
bool measure_dirty(const ui2::TextInput& old, const ui2::TextInput& next) {
    return !text_measure_equal(old.text_style, next.text_style);
}
bool measure_dirty(const ui2::ListView& old, const ui2::ListView& next) {
    return !same_items(old.items, next.items) || old.row_height != next.row_height || old.row_spacing != next.row_spacing || old.wrap != next.wrap ||
           !text_measure_equal(old.text_style, next.text_style);
}
bool measure_dirty(const ui2::TabBar& old, const ui2::TabBar& next) {
    return !same_tab_items(old.items, next.items) ||
           old.tab_height != next.tab_height ||
           old.min_tab_width != next.min_tab_width ||
           old.max_tab_width != next.max_tab_width ||
           old.gap != next.gap ||
           old.wrap != next.wrap ||
           !text_measure_equal(old.text_style, next.text_style);
}
bool measure_dirty(const ui2::MenuList& old, const ui2::MenuList& next) {
    return !same_menu_items(old.items, next.items) ||
           old.row_height != next.row_height ||
           old.row_spacing != next.row_spacing ||
           old.wrap != next.wrap ||
           !text_measure_equal(old.text_style, next.text_style);
}

template <typename T, typename... Members>
void preserve_runtime_fields(T& next, const T& old, Members... members) {
    ((next.*members = old.*members), ...);
}

// These widgets carry meaningful state in addition to their authored config.
// Keep the state lists next to the sync boundary so replacing a component
// does not accidentally reset an active edit, selection, or menu interaction.
void preserve_text_input_state(ui2::TextInput& next, const ui2::TextInput& old) {
    preserve_runtime_fields(next, old, &ui2::TextInput::state, &ui2::TextInput::result);
}

void preserve_list_view_state(ui2::ListView& next, const ui2::ListView& old) {
    preserve_runtime_fields(next, old,
                           &ui2::ListView::selected,
                           &ui2::ListView::offset,
                           &ui2::ListView::first,
                           &ui2::ListView::visible,
                           &ui2::ListView::drag_source,
                           &ui2::ListView::dragging,
                           &ui2::ListView::drop_target,
                           &ui2::ListView::dropped_source,
                           &ui2::ListView::dropped_target,
                           &ui2::ListView::drag_started,
                           &ui2::ListView::dropped,
                           &ui2::ListView::activated);
}

void preserve_menu_list_state(ui2::MenuList& next, const ui2::MenuList& old) {
    preserve_runtime_fields(next, old,
                           &ui2::MenuList::selected,
                           &ui2::MenuList::scroll_offset,
                           &ui2::MenuList::activated,
                           &ui2::MenuList::activated_id,
                           &ui2::MenuList::hovered,
                           &ui2::MenuList::hovered_id,
                           &ui2::MenuList::hovered_rect,
                           &ui2::MenuList::submenu_requested,
                           &ui2::MenuList::submenu_id,
                           &ui2::MenuList::submenu_anchor,
                           &ui2::MenuList::content_height,
                           &ui2::MenuList::viewport_height,
                           &ui2::MenuList::scroll_changed,
                           &ui2::MenuList::cancelled);
}

template <typename T>
void preserve_runtime_state(T& next, const T& old) {
    if constexpr (requires { next.id; }) {
        next.id = old.id;
    }
    if constexpr (requires { next.bounds; }) {
        next.bounds = old.bounds;
    }
    if constexpr (requires { next.interaction; }) {
        next.interaction = old.interaction;
    }
    if constexpr (requires { next.clicked; }) {
        next.clicked = old.clicked;
    }
    if constexpr (requires { next.changed; }) {
        next.changed = old.changed;
    }

    if constexpr (std::is_same_v<T, ui2::TextInput>) {
        preserve_text_input_state(next, old);
    } else if constexpr (std::is_same_v<T, ui2::ListView>) {
        preserve_list_view_state(next, old);
    } else if constexpr (std::is_same_v<T, ui2::TabBar>) {
        preserve_runtime_fields(next, old,
                               &T::selected,
                               &T::offset,
                               &T::activated,
                               &T::activated_id,
                               &T::closed,
                               &T::closed_id);
    } else if constexpr (std::is_same_v<T, ui2::MenuList>) {
        preserve_menu_list_state(next, old);
    }
}

bool scroll_config_dirty(const Ui2ScrollContainer& old, const Ui2ScrollContainer& next) {
    return old.wheel_step != next.wheel_step ||
           old.wheel_step_x != next.wheel_step_x ||
           old.scrollbar_thickness != next.scrollbar_thickness ||
           old.min_thumb != next.min_thumb ||
           old.show_scrollbar != next.show_scrollbar ||
           old.horizontal != next.horizontal;
}

Ui2ScrollContainer merge_scroll_config(Ui2ScrollContainer old, const Ui2ScrollContainer& next) {
    old.wheel_step = next.wheel_step;
    old.wheel_step_x = next.wheel_step_x;
    old.scrollbar_thickness = next.scrollbar_thickness;
    old.min_thumb = next.min_thumb;
    old.show_scrollbar = next.show_scrollbar;
    old.horizontal = next.horizontal;
    old.scrollbar_track = next.scrollbar_track;
    old.scrollbar_thumb = next.scrollbar_thumb;
    return old;
}

} // namespace

Ui2Sync::Ui2Sync(flecs::world& world, std::string_view name, Rectf bounds)
    : _world(&world) {
    _root = EcsEntity{world.entity(std::string{name}.c_str())};
    bool dirty = false;
    if (const auto* root = _root.get<Ui2Root>()) {
        dirty = root->bounds != bounds;
    } else {
        dirty = true;
    }
    if (dirty) {
        _root.set(Ui2Root{.bounds = bounds});
    }
    _root.raw().ensure<Ui2LayoutCache>();
    _index = &_root.raw().ensure<Ui2SyncIndex>();
    _frame = ++_index->frame;
    const u64 root_key = ui2::make_id(ui2::make_id("ecs.ui2.sync"), name).value;
    _stack.push_back({.parent = _root, .key = root_key});
    if (dirty) {
        mark_dirty();
    }
}

Ui2Sync& Ui2Sync::key(std::string_view value) {
    _next_key.assign(value.data(), value.size());
    return *this;
}

Ui2Sync& Ui2Sync::keep_unvisited(bool enabled) {
    if (_index) {
        _index->keep_unvisited = enabled;
    }
    return *this;
}

Ui2Sync& Ui2Sync::layout(ui2::LayoutStyle style) {
    _next_layout = style;
    _has_next_layout = true;
    return *this;
}

u64 Ui2Sync::next_key(std::string_view kind, std::string_view fallback) {
    const ui2::Id parent{_stack.empty() ? 0 : _stack.back().key};
    if (!_next_key.empty()) {
        const u64 key = ui2::make_id(parent, _next_key).value;
        _next_key.clear();
        return key;
    }

    ui2::Id key = ui2::make_id(parent, kind);
    if (!fallback.empty()) {
        key = ui2::make_id(key, fallback);
    }
    u32& occurrence = _stack.back().occurrences[key.value];
    const u32 current = occurrence++;
    return ui2::make_id(key, static_cast<u64>(current)).value;
}

void Ui2Sync::visit(EcsEntity entity, u64 key) {
    entity.set(Ui2SyncKey{.key = key, .last_visit = _frame});
    _index->by_key[key] = entity.id();
}

void Ui2Sync::mark_dirty() {
    ui2_mark_dirty(_root);
}

EcsEntity Ui2Sync::upsert(std::string_view kind, std::string_view fallback, const ui2::LayoutStyle* style) {
    const u64 key = next_key(kind, fallback);
    EcsEntity entity;
    const auto found = _index->by_key.find(key);
    if (found != _index->by_key.end()) {
        entity = EcsEntity{_world->entity(found->second)};
        if (!entity) {
            entity = {};
        }
    }
    bool created = false;
    if (!entity) {
        entity = EcsEntity{_world->entity()};
        created = true;
    }

    const EcsEntity parent = _stack.back().parent;
    if (entity.parent().id() != parent.id()) {
        entity.child_of(parent);
        mark_dirty();
    }

    ui2::LayoutStyle desired = style ? *style : ui2::LayoutStyle{};
    if (!style && _has_next_layout) {
        desired = _next_layout;
    }
    _has_next_layout = false;
    const u32 sequence = _stack.back().next_sequence++;
    if (const auto* current = entity.get<Ui2Layout>()) {
        if (!ecs_ui2_detail::layout_style_equal(current->style, desired) || current->sequence != sequence) {
            entity.set(Ui2Layout{.style = desired, .sequence = sequence});
            mark_dirty();
        }
    } else {
        entity.set(Ui2Layout{.style = desired, .sequence = sequence});
        mark_dirty();
    }

    visit(entity, key);
    if (created) {
        mark_dirty();
    }
    return entity;
}

void Ui2Sync::begin_column(ui2::LayoutStyle style) {
    EcsEntity entity = upsert("column", {}, &style);
    const Ui2SyncKey* key = entity.get<Ui2SyncKey>();
    _stack.push_back({.parent = entity, .key = key ? key->key : 0});
}

void Ui2Sync::begin_row(ui2::LayoutStyle style) {
    EcsEntity entity = upsert("row", {}, &style);
    const Ui2SyncKey* key = entity.get<Ui2SyncKey>();
    _stack.push_back({.parent = entity, .key = key ? key->key : 0});
}

void Ui2Sync::begin_scroll(ui2::LayoutStyle style, Ui2ScrollContainer scroll) {
    EcsEntity entity = upsert("scroll", {}, &style);
    bool dirty = false;
    if (const auto* current = entity.get<Ui2ScrollContainer>()) {
        dirty = scroll_config_dirty(*current, scroll);
        if (dirty) {
            entity.set(merge_scroll_config(*current, scroll));
        }
    } else {
        entity.set(scroll);
        dirty = true;
    }
    if (dirty) {
        mark_dirty();
    }
    const Ui2SyncKey* key = entity.get<Ui2SyncKey>();
    _stack.push_back({.parent = entity, .key = key ? key->key : 0});
}

void Ui2Sync::end_container() {
    if (_stack.size() > 1) {
        _stack.pop_back();
    }
}

template <typename T>
T Ui2Sync::sync_widget(std::string_view kind, std::string_view fallback, T widget) {
    EcsEntity entity = upsert(kind, fallback, nullptr);
    remove_other_v1_widgets<T>(entity);
    bool dirty = true;
    if (const T* current = entity.get<T>()) {
        T next = std::move(widget);
        dirty = measure_dirty(*current, next);
        preserve_runtime_state(next, *current);
        entity.set(std::move(next));
    } else {
        entity.set(std::move(widget));
    }
    if (dirty) {
        mark_dirty();
    }
    if (const T* current = entity.get<T>()) {
        return *current;
    }
    return {};
}

ui2::Panel Ui2Sync::panel(ui2::Panel widget) { return sync_widget("panel", {}, std::move(widget)); }
ui2::Label Ui2Sync::label(ui2::Label widget) { return sync_widget("label", widget.text, std::move(widget)); }
ui2::WrappedText Ui2Sync::wrapped_text(ui2::WrappedText widget) { return sync_widget("wrapped_text", widget.text, std::move(widget)); }
ui2::Separator Ui2Sync::separator(ui2::Separator widget) { return sync_widget("separator", {}, std::move(widget)); }
ui2::Button Ui2Sync::button(ui2::Button widget) { return sync_widget("button", widget.label, std::move(widget)); }
ui2::Toggle Ui2Sync::toggle(ui2::Toggle widget) { return sync_widget("toggle", widget.label, std::move(widget)); }
ui2::Slider Ui2Sync::slider(ui2::Slider widget) { return sync_widget("slider", {}, std::move(widget)); }
ui2::ProgressBar Ui2Sync::progress(ui2::ProgressBar widget) { return sync_widget("progress", {}, std::move(widget)); }
ui2::Image Ui2Sync::image(ui2::Image widget) { return sync_widget("image", {}, std::move(widget)); }
ui2::Spacer Ui2Sync::spacer(ui2::Spacer widget) { return sync_widget("spacer", {}, std::move(widget)); }
ui2::IconButton Ui2Sync::icon_button(ui2::IconButton widget) { return sync_widget("icon_button", widget.label, std::move(widget)); }
ui2::Meter Ui2Sync::meter(ui2::Meter widget) { return sync_widget("meter", {}, std::move(widget)); }
ui2::TextInput Ui2Sync::text_input(ui2::TextInput widget) { return sync_widget("text_input", {}, std::move(widget)); }
ui2::ListView Ui2Sync::list_view(ui2::ListView widget) { return sync_widget("list_view", {}, std::move(widget)); }
ui2::TabBar Ui2Sync::tab_bar(ui2::TabBar widget) { return sync_widget("tab_bar", {}, std::move(widget)); }
ui2::MenuList Ui2Sync::menu_list(ui2::MenuList widget) { return sync_widget("menu_list", {}, std::move(widget)); }

void Ui2Sync::end() {
    if (_ended || !_index) {
        return;
    }
    _ended = true;
    if (_index->keep_unvisited) {
        return;
    }

    bool destroyed = false;
    for (auto it = _index->by_key.begin(); it != _index->by_key.end();) {
        EcsEntity entity{_world->entity(it->second)};
        const Ui2SyncKey* key = entity ? entity.get<Ui2SyncKey>() : nullptr;
        if (!entity || !key || key->last_visit != _frame) {
            if (entity) {
                entity.destroy();
                destroyed = true;
            }
            it = _index->by_key.erase(it);
        } else {
            ++it;
        }
    }
    if (destroyed) {
        mark_dirty();
    }
}

Ui2Sync begin_ui2_sync(EcsWorld& world, std::string_view name, Rectf bounds) {
    return begin_ui2_sync(world.raw(), name, bounds);
}

Ui2Sync begin_ui2_sync(flecs::world& world, std::string_view name, Rectf bounds) {
    return Ui2Sync{world, name, bounds};
}

} // namespace kin
