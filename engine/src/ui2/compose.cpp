// Composition (experimental): open containers for the immediate layout builder.
//
// Boxes are layout nodes the Context tags (_compose). Building one hit-tests the
// rect it was drawn at last frame (_compose_records), so the caller gets hovered /
// clicked while still declaring its children; end_layout() solves the tree, moves
// scroll content (layout_compose), draws every node under its boxes' clips and
// records the rects for the next frame (record_compose). Focus order and key
// bindings ride on the same records: a box knows its parent box, so focus has a
// path to the root.

#include <kin/ui2/context.hpp>

#include <kin/assets/image.hpp>

#include <algorithm>
#include <cmath>

namespace kin::ui2 {

namespace {

// Records of ids not built for this many frames are dropped.
constexpr u64 stale_frames = 120;

Rectf intersect(Rectf a, Rectf b) {
    const f32 x0 = std::max(a.x, b.x);
    const f32 y0 = std::max(a.y, b.y);
    const f32 x1 = std::min(a.x + a.w, b.x + b.w);
    const f32 y1 = std::min(a.y + a.h, b.y + b.h);
    return {x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0)};
}

bool horizontal(UiLayoutAxis axis) {
    return axis == UiLayoutAxis::Horizontal;
}

// Axis 0 is vertical (y), 1 horizontal (x), as ScrollRecord's arrays.
f32& along(Vec2f& v, i32 axis) {
    return axis == 0 ? v.y : v.x;
}

f32 along(const Vec2f& v, i32 axis) {
    return axis == 0 ? v.y : v.x;
}

ScrollAxis scroll_axis(i32 axis) {
    return axis == 0 ? ScrollAxis::Vertical : ScrollAxis::Horizontal;
}

bool has(KeyModifiers value, KeyModifiers modifier) {
    return (value & modifier) != KeyModifiers::None;
}

bool exact_modifiers(const Context& ctx, KeyModifiers wanted) {
    for (const KeyModifiers m : {KeyModifiers::Shift, KeyModifiers::Ctrl, KeyModifiers::Alt}) {
        if (ctx.modifier_held(m) != has(wanted, m)) {
            return false;
        }
    }
    return true;
}

} // namespace

void Context::label(std::string_view text, LayoutStyle style) {
    label(text, _theme.body_text, style);
}

void Context::label(std::string_view text, const TextStyle& text_style, LayoutStyle style, TextOverflow overflow) {
    if (overflow != TextOverflow::Visible && overflow != TextOverflow::Clip) {
        style.shrink = true; // as wide as its text, less (and cut) when its line is too long
    }
    add(Label{.text = std::string(text), .text_style = text_style, .overflow = overflow}, style);
}

void Context::image(const Sprite& sprite, Vec2f size, Color tint, LayoutStyle style) {
    add(Image{.sprite = sprite, .size = size, .tint = tint}, style);
}

void Context::svg(std::string_view svg, Vec2f size, Color tint, SvgOptions options, LayoutStyle style) {
    image(svg_sprite(svg, size, options), size, tint, style);
}

const Sprite& Context::svg_sprite(std::string_view svg, Vec2f size, SvgOptions options) {
    static const Sprite none{};
    if (options.oversample <= 0.0f) {
        options.oversample = _pixel_scale > 0.0f ? _pixel_scale : 1.0f; // the renderer's pixels per unit
    }
    const Vec2i pixels{static_cast<i32>(std::lround(size.x * options.oversample)),
                       static_cast<i32>(std::lround(size.y * options.oversample))};
    if (!_renderer || svg.empty() || pixels.x <= 0 || pixels.y <= 0) {
        return none;
    }
    u64 key = std::hash<std::string_view>{}(svg);
    for (const u64 part : {static_cast<u64>(pixels.x), static_cast<u64>(pixels.y), static_cast<u64>(options.mask)}) {
        key = (key ^ part) * 0x100000001b3ull;
    }
    const auto found = _svg_sprites.find(key);
    if (found != _svg_sprites.end()) {
        return found->second;
    }
    kin::Image image = rasterize_svg(svg, pixels); // not ui2::Image, the widget
    Sprite& sprite = _svg_sprites[key]; // an unreadable one is remembered too, empty
    if (image.valid()) {
        if (options.mask) {
            for (std::size_t i = 0; i < image.rgba.size(); i += 4) {
                image.rgba[i] = image.rgba[i + 1] = image.rgba[i + 2] = 255;
            }
        }
        sprite = full_sprite(_renderer->create_texture_from_rgba(image.rgba.data(), image.size));
    }
    return sprite;
}

void Context::spacer(LayoutStyle style) {
    add(Spacer{}, style);
}

// --- boxes ---------------------------------------------------------------------------

BoxResult Context::compose_interaction(Id id, const BoxStyle& style) {
    BoxResult out;
    if (!id) {
        return out;
    }
    if (style.focusable && style.enabled) {
        add_focusable(id, style.tab_index, true);
    }
    const auto record = _compose_records.find(id.value);
    if (record == _compose_records.end()) {
        return out; // first frame: nowhere to hit yet
    }
    out.bounds = record->second.rect;
    const Rectf visible = record->second.visible;
    out.pressed_outside = _any_press && !contains(visible, pointer()) && !press_inside(id);
    if (!style.interactive || !style.enabled || !_input) {
        return out;
    }
    static_cast<Interaction&>(out) = region(id, visible, compose_z(style));
    if (!style.drag_type.empty()) {
        if (out.pressed) {
            _drag = {.source = id,
                     .start = pointer(),
                     .payload = {.type = std::string(style.drag_type), .text = std::string(style.drag_text), .value = style.drag_value},
                     .label = true};
        }
        const Vec2f moved{pointer().x - _drag.start.x, pointer().y - _drag.start.y};
        if (_drag.source == id && pointer_held() && !_drag.dragging && moved.x * moved.x + moved.y * moved.y >= 16.0f) {
            _drag.dragging = true;
            _drag.started_this_frame = true;
        }
        if (_drag.source == id && pointer_released()) {
            _drag.released_this_frame = true;
        }
        out.drag_started = _drag.source == id && _drag.started_this_frame;
        out.dragging = _drag.source == id && _drag.dragging;
        if (out.dragging) {
            out.clicked = false; // let go where it began: still a drag, not a click
        }
    }
    if (out.clicked) {
        out.double_clicked = _state.register_click(id, pointer(), _now);
    }
    if (out.hot && pointer_released(MouseButton::Right)) {
        out.secondary_clicked = true;
    }
    // Enter or Space on the focused box clicks it, unless a text field has the keys.
    if (style.focusable && out.focused && !_wants_text_input_last &&
        (key_pressed(Key::Enter) || key_pressed(Key::KeypadEnter) || key_pressed(Key::Space))) {
        out.clicked = true;
    }

    if (_modal_blocking) {
        const Rectf allowed = !_modal_stack.empty() ? _modal_stack.back() : _modal_bounds;
        if (!contains(allowed, screen_pointer())) {
            return out;
        }
    }
    if (!contains(visible, pointer())) {
        return out;
    }
    // Over the box. It is hovered unless last frame's topmost region was another
    // recorded box that is not inside this one (a sibling, or a box on top), or a
    // widget hit above it (a field in a dialog over it).
    out.hovered = true;
    const Id hot = _state.hot();
    if (hot && hot != id) {
        auto it = _compose_records.find(hot.value);
        if (it == _compose_records.end() && _state.hot_z() > compose_z(style)) {
            out.hovered = false;
        }
        // A box hit last frame that the pointer has left since (it moved to
        // this one, its neighbour) is not over this one: no frame unlit between.
        if (it != _compose_records.end() && !contains(it->second.visible, pointer())) {
            it = _compose_records.end();
        }
        if (it != _compose_records.end()) {
            out.hovered = false;
            for (std::size_t guard = 0; it != _compose_records.end() && guard < 256; ++guard) {
                if (it->second.parent == id) {
                    out.hovered = true;
                    break;
                }
                it = it->second.parent ? _compose_records.find(it->second.parent.value) : _compose_records.end();
            }
        }
    }
    if (!style.drop_type.empty() && _drag.dragging && (style.drop_type == "*" || _drag.payload.type == style.drop_type) && out.hovered) {
        out.drop_hovered = true;
        if (pointer_released()) {
            out.dropped = true;
            _drag.released_this_frame = true;
        }
    }
    if (!style.tooltip_text.empty()) {
        show_tooltip(make_id(id, "tooltip"), out.hovered && !_drag.dragging && !_state.active(), visible, style.tooltip_text,
                     tooltip_style());
    }
    return out;
}

// Hit order: a floating box, and everything in it, above what is not floating
// (and a box floating in a floating box above that).
i32 Context::compose_z(const BoxStyle& style) const {
    i32 z = style.z + (style.layout.floats ? floating_hit_z : 0);
    for (const i32 open : _stack) {
        if (_nodes[static_cast<std::size_t>(open)].style.floats) {
            z += floating_hit_z;
        }
    }
    return z;
}

BoxStyle& BoxStyle::bg(Color color) {
    const SurfaceStyle before = surface.normal;
    for (SurfaceStyle* state : {&surface.normal, &surface.hovered, &surface.pressed, &surface.focused, &surface.disabled, &surface.selected}) {
        // A state with a fill of its own keeps it: one never set, or still the old bg, takes this.
        if (state == &surface.normal || !state->draw_fill || state->fill == before.fill) {
            state->fill = color;
            state->draw_fill = true;
        }
    }
    draw_surface = true;
    return *this;
}

BoxStyle& BoxStyle::hover_bg(Color color) {
    for (SurfaceStyle* state : {&surface.hovered, &surface.pressed}) {
        state->fill = color;
        state->draw_fill = true;
    }
    draw_surface = true;
    return *this;
}

BoxStyle& BoxStyle::pressed_bg(Color color) {
    surface.pressed.fill = color;
    surface.pressed.draw_fill = true;
    draw_surface = true;
    return *this;
}

BoxStyle& BoxStyle::selected_bg(Color color) {
    surface.selected.fill = color;
    surface.selected.draw_fill = true;
    draw_surface = true;
    return *this;
}

BoxStyle& BoxStyle::rounded(f32 radius) {
    for (SurfaceStyle* state : {&surface.normal, &surface.hovered, &surface.pressed, &surface.focused, &surface.disabled, &surface.selected}) {
        state->radius = radius;
    }
    return *this;
}

BoxStyle& BoxStyle::border(Color color, f32 width) {
    for (SurfaceStyle* state : {&surface.normal, &surface.hovered, &surface.pressed, &surface.focused, &surface.disabled, &surface.selected}) {
        state->border = color;
        state->border_width = width;
        state->border_mode = BorderMode::Inside;
    }
    draw_surface = true;
    return *this;
}

BoxStyle& BoxStyle::focus_border(Color color, f32 width) {
    surface.focused.border = color;
    surface.focused.border_width = width;
    surface.focused.border_mode = BorderMode::Inside;
    draw_surface = true;
    return *this;
}

void Context::begin_compose(Id id, ComposeKind kind, const BoxStyle& style, LayoutStyle layout) {
    layout.draw_surface = false; // a box draws its own (interactive) surface
    const i32 index = add_node(layout, {});
    _stack.push_back(index);
    _compose.push_back({
        .node = index,
        .id = id,
        .kind = kind,
        .clip = style.clip || style.clip_rounded,
        .clip_radius = style.clip_rounded ? style.surface.normal.radius : 0.0f,
        .key_context = std::string(style.key_context),
        .interactive = style.interactive,
        .window_drag = style.window_drag,
        .z = compose_z(style),
    });
    _compose_ids.push_back(id);
    if (style.traps_focus && id) {
        _focus_trap = id;
    }
}

BoxResult Context::begin_box(Id id, const BoxStyle& style) {
    const BoxResult result = compose_interaction(id, style);
    if (style.cursor != SystemCursor::Default && (result.hovered || result.active)) {
        set_cursor(style.cursor); // a box inside, built later, may set its own
    }
    begin_compose(id, ComposeKind::Box, style, style.layout);
    if (style.draw_surface) {
        // The surface looks hovered over the box's children too, and is resolved now
        // (the interaction is known) so the draw keeps one SurfaceStyle, not six.
        Interaction look = result;
        look.hot = result.hovered;
        _pending.add(_stack.back(),
                     [surface = resolve_animated(id, style.surface, look, style.selected, style.enabled)](
                         Context& ctx, Rectf bounds) { ctx.surface(bounds, surface); });
    }
    return result;
}

bool Context::press_inside(Id box) const {
    auto it = _compose_records.find(_state.hot().value);
    for (std::size_t guard = 0; it != _compose_records.end() && guard < 256; ++guard) {
        if (it->first == box.value) {
            return true;
        }
        it = it->second.parent ? _compose_records.find(it->second.parent.value) : _compose_records.end();
    }
    return false;
}

std::optional<Context::BoxScope> Context::popup(Id id, BoxStyle style, const PopupPlace& place) {
    if (!popup_open(id)) {
        return std::nullopt;
    }
    // Closed by a press outside it, its submenu and its opener; by Esc on top.
    const auto inside = [&](Id box) {
        if (!box) {
            return false;
        }
        const auto record = _compose_records.find(box.value);
        return (record != _compose_records.end() && contains(record->second.visible, pointer())) || press_inside(box);
    };
    const bool submenu_of_it = _open_popup == id && _open_subpopup;
    const bool on_opener = inside(place.opener) || (place.opener_bounds.w > 0.0f && contains(place.opener_bounds, pointer()));
    if (_any_press && _popup_opened_this_frame != id && !inside(id) && !on_opener && !(submenu_of_it && inside(_open_subpopup))) {
        close_popup(id);
        return std::nullopt;
    }
    if (popup_on_top(id) && (key_pressed(Key::Escape) || action_pressed("quit"))) {
        close_popup(id);
        return std::nullopt;
    }
    Vec2f at = place.point ? *place.point : _popup_point.value_or(pointer());
    if (place.anchor.w > 0.0f || place.anchor.h > 0.0f) {
        at = place.side == UiFloat::Right ? Vec2f{place.anchor.x + place.anchor.w, place.anchor.y}
                                          : Vec2f{place.anchor.x, place.anchor.y + place.anchor.h};
    }
    style.floating(UiFloat::Point, {at.x + place.offset.x, at.y + place.offset.y});
    return BoxScope(this, begin_box(id, style), &Context::end_box);
}

void Context::end_box() {
    end_container();
    if (!_compose_ids.empty()) {
        _compose_ids.pop_back();
    }
}

// --- scroll boxes --------------------------------------------------------------------

BoxResult Context::begin_scroll(Id id, const BoxStyle& style, ScrollBoxOptions options) {
    BoxStyle box = style;
    box.clip = true;
    box.layout.scroll_y = options.vertical;
    box.layout.scroll_x = options.horizontal;
    _scroll_records.try_emplace(id.value);
    scroll_thumbs(id, options, compose_z(style) + 1); // before the content, so a drag moves it this frame
    const BoxResult result = begin_box(id, box);
    _compose.back().kind = ComposeKind::Scroll;
    _compose.back().scroll = options;
    _scroll_ids.push_back(id);
    return result;
}

// The thumbs as drawn last frame, dragged now: the offset they set places this
// frame's content.
void Context::scroll_thumbs(Id id, const ScrollBoxOptions& options, i32 z) {
    ScrollRecord& record = _scroll_records[id.value];
    for (i32 axis = 0; axis < 2; ++axis) {
        record.thumb[static_cast<std::size_t>(axis)] = {};
        const Rectf track = record.track[static_cast<std::size_t>(axis)];
        const bool enabled = axis == 0 ? options.vertical : options.horizontal;
        if (!enabled || !options.show_scrollbar || track.w <= 0.0f || track.h <= 0.0f) {
            continue;
        }
        const f32 max_offset = std::max(0.0f, along(record.content, axis) - along(record.viewport, axis));
        if (max_offset <= 0.5f) {
            continue;
        }
        const ScrollbarLayout bar = layout_scrollbar(track,
                                                     {.offset = record.offset, .content_size = record.content, .viewport_size = record.viewport},
                                                     scroll_axis(axis),
                                                     {.scrollbar_thickness = options.scrollbar_thickness, .min_thumb = options.min_thumb});
        const Interaction it = region(make_id(id, axis == 0 ? "thumb_y" : "thumb_x"), bar.thumb, z);
        record.thumb[static_cast<std::size_t>(axis)] = it;
        f32& grab = record.grab[static_cast<std::size_t>(axis)];
        const Vec2f p = pointer();
        const f32 thumb_start = axis == 0 ? bar.thumb.y : bar.thumb.x;
        if (it.pressed) {
            grab = along(p, axis) - thumb_start;
        }
        if (it.active && grab >= 0.0f) {
            const f32 travel = axis == 0 ? track.h - bar.thumb.h : track.w - bar.thumb.w;
            const f32 start = axis == 0 ? track.y : track.x;
            if (travel > 0.0f) {
                along(record.offset, axis) = std::clamp((along(p, axis) - grab - start) / travel, 0.0f, 1.0f) * max_offset;
            }
        } else if (!it.active) {
            grab = -1.0f;
        }
    }
}

void Context::end_scroll() {
    if (_scroll_ids.empty()) {
        end_box();
        return;
    }
    const Id id = _scroll_ids.back();
    _scroll_ids.pop_back();
    const auto open = std::find_if(_compose.rbegin(), _compose.rend(), [&](const ComposeNode& node) {
        return node.kind == ComposeKind::Scroll && node.id == id;
    });
    const ScrollBoxOptions options = open != _compose.rend() ? open->scroll : ScrollBoxOptions{};
    if (options.show_scrollbar && options.scrollbar_thickness > 0.0f && !_stack.empty()) {
        // Overlay children along the right and bottom edges, out of the box's
        // padding; they do not scroll. With both, each stops short of the corner.
        const UiPadding pad = _nodes[static_cast<std::size_t>(_stack.back())].style.padding;
        const f32 t = options.scrollbar_thickness;
        const auto add_bar = [&](ComposeKind kind, i32 axis, LayoutStyle bar) {
            const i32 index = add_node(bar, {});
            _compose.push_back({.node = index, .id = id, .kind = kind, .scroll = options});
            _pending.add(index, [id, axis, options](Context& ctx, Rectf track) {
                ctx.draw_scrollbar(id, track, axis, options);
            });
        };
        if (options.vertical) {
            add_bar(ComposeKind::ScrollbarY, 0, {
                .width = fixed(t),
                .height = grow(),
                .margin = {0.0f, -pad.top, -pad.right, options.horizontal ? t - pad.bottom : -pad.bottom},
                .overlay = true,
                .anchor = UiAnchor::TopRight,
            });
        }
        if (options.horizontal) {
            add_bar(ComposeKind::ScrollbarX, 1, {
                .width = grow(),
                .height = fixed(t),
                .margin = {-pad.left, 0.0f, options.vertical ? t - pad.right : -pad.right, -pad.bottom},
                .overlay = true,
                .anchor = UiAnchor::BottomLeft,
            });
        }
    }
    end_box();
}

Vec2f Context::scroll_offset(Id id) const {
    const auto it = _scroll_records.find(id.value);
    return it != _scroll_records.end() ? it->second.offset : Vec2f{};
}

void Context::set_scroll_offset(Id id, Vec2f offset) {
    _scroll_records[id.value].offset = {std::max(0.0f, offset.x), std::max(0.0f, offset.y)};
}

void Context::draw_scrollbar(Id id, Rectf track, i32 axis, const ScrollBoxOptions& options) {
    const auto found = _scroll_records.find(id.value);
    if (found == _scroll_records.end()) {
        return;
    }
    ScrollRecord& record = found->second;
    record.track[static_cast<std::size_t>(axis)] = track; // where scroll_thumbs() hits next frame
    if (along(record.content, axis) <= along(record.viewport, axis) + 0.5f) {
        return;
    }
    const ScrollbarLayout bar = layout_scrollbar(track,
                                                 {.offset = record.offset, .content_size = record.content, .viewport_size = record.viewport},
                                                 scroll_axis(axis),
                                                 {.scrollbar_thickness = options.scrollbar_thickness, .min_thumb = options.min_thumb});
    const Interaction& it = record.thumb[static_cast<std::size_t>(axis)];
    surface(track, _theme.scrollbar_track_surface);
    SurfaceStyle thumb = _theme.scrollbar_thumb_surface;
    if (!it.hot && !it.active) {
        thumb.opacity *= 0.7f;
    }
    surface(bar.thumb, thumb);
}

// --- lists ---------------------------------------------------------------------------

ListRange Context::begin_list(Id id, i32 count, f32 row_height, LayoutStyle style) {
    count = std::max(0, count);
    row_height = std::max(1.0f, row_height);
    style.height = fixed(static_cast<f32>(count) * row_height);
    style.axis = UiLayoutAxis::Vertical;
    style.spacing = 0.0f;
    style.wrap = false;
    style.grid_columns = 0;

    // Where the rows can be seen: the enclosing scroll box, else the layout root,
    // both as of last frame. A wheel turn this frame is covered by overscan.
    Rectf view = _root;
    f32 wheel_step = 48.0f;
    Vec2f moved{}; // how far the scroll box's offset moved since its content was placed
    if (!_scroll_ids.empty()) {
        const Id scroll = _scroll_ids.back();
        if (const auto it = _compose_records.find(scroll.value); it != _compose_records.end()) {
            view = it->second.visible;
        }
        // A thumb dragged, set_scroll_offset(), a row revealed: this frame's rows
        // are where the new offset puts them.
        if (const auto it = _scroll_records.find(scroll.value); it != _scroll_records.end()) {
            moved = {it->second.offset.x - it->second.laid_out.x, it->second.offset.y - it->second.laid_out.y};
        }
        for (auto node = _compose.rbegin(); node != _compose.rend(); ++node) {
            if (node->kind == ComposeKind::Scroll && node->id == scroll) {
                wheel_step = node->scroll.wheel_step;
                break;
            }
        }
    }
    const i32 overscan = 2 + static_cast<i32>(std::ceil(std::abs(mouse_wheel_y()) * wheel_step / row_height));
    ListRange range{0, std::min(count, static_cast<i32>(std::ceil(view.h / row_height)) + overscan)};
    if (const auto it = _compose_records.find(id.value); it != _compose_records.end()) {
        const f32 top = it->second.rect.y - moved.y;
        range.first = static_cast<i32>(std::floor((view.y - top) / row_height)) - overscan;
        range.last = static_cast<i32>(std::ceil((view.y + view.h - top) / row_height)) + overscan;
        range.first = std::clamp(range.first, 0, count);
        range.last = std::clamp(range.last, range.first, count);
    }

    begin_compose(id, ComposeKind::List, BoxStyle{.interactive = false}, style);
    _compose.back().row_height = row_height;
    _list_heights.push_back(row_height);
    if (range.first > 0) {
        add_node({.width = grow(), .height = fixed(static_cast<f32>(range.first) * row_height)}, {});
    }
    return range;
}

void Context::begin_list_row() {
    const f32 height = !_list_heights.empty() ? _list_heights.back() : 24.0f;
    begin_container({.width = grow(), .height = fixed(height), .axis = UiLayoutAxis::Horizontal, .cross = UiAlign::Center});
}

void Context::end_list() {
    if (!_list_heights.empty()) {
        _list_heights.pop_back();
    }
    end_box();
}

void Context::scroll_to_row(Id list, i32 row) {
    reveal({list, row});
}

void Context::scroll_into_view(Id id) {
    reveal({id, -1});
}

// --- splits --------------------------------------------------------------------------

void Context::begin_split(Id id, SplitOptions options) {
    f32& ratio = _split_ratios.try_emplace(id.value, std::clamp(options.ratio, 0.0f, 1.0f)).first->second;
    const bool across = horizontal(options.axis);
    const Id handle_id = make_id(id, "handle");

    Interaction handle;
    const auto outer = _compose_records.find(id.value);
    if (const auto it = _compose_records.find(handle_id.value); it != _compose_records.end()) {
        handle = region(handle_id, it->second.visible);
    }
    if (handle.hot || handle.active) {
        set_cursor(across ? SystemCursor::ResizeEW : SystemCursor::ResizeNS);
    }
    if (handle.active && outer != _compose_records.end()) {
        const Rectf r = outer->second.rect;
        const f32 total = across ? r.w : r.h;
        if (total > 0.0f) {
            const f32 at = (across ? pointer().x - r.x : pointer().y - r.y) - options.thickness * 0.5f;
            const f32 lo = options.min_first / total;
            const f32 hi = (total - options.min_second - options.thickness) / total;
            ratio = std::clamp(at / total, 0.0f, 1.0f);
            ratio = hi > lo ? std::clamp(ratio, lo, hi) : lo;
        }
    }

    LayoutStyle layout = options.layout;
    layout.axis = options.axis;
    layout.spacing = 0.0f;
    begin_compose(id, ComposeKind::Split, BoxStyle{.interactive = false}, layout);
    _splits_open.push_back({.id = id, .options = options, .handle = handle});

    LayoutStyle first;
    (across ? first.width : first.height) = percent(ratio);
    (across ? first.height : first.width) = grow();
    begin_compose({}, ComposeKind::Box, BoxStyle{.interactive = false, .clip = true}, first);
}

void Context::split_next() {
    if (_splits_open.empty()) {
        return;
    }
    end_box(); // the first pane
    const SplitFrame& split = _splits_open.back();
    const bool across = horizontal(split.options.axis);

    LayoutStyle bar;
    (across ? bar.width : bar.height) = fixed(split.options.thickness);
    (across ? bar.height : bar.width) = grow();
    const i32 index = add_node(bar, {});
    _compose.push_back({.node = index, .id = make_id(split.id, "handle"), .kind = ComposeKind::SplitHandle});
    _pending.add(index, [it = split.handle](Context& ctx, Rectf bounds) {
                            SurfaceStyle look = ctx.theme().scrollbar_track_surface;
                            if (it.hot || it.active) {
                                look = ctx.theme().scrollbar_thumb_surface;
                            }
                            ctx.surface(bounds, look);
                        });

    LayoutStyle second;
    second.width = grow();
    second.height = grow();
    begin_compose({}, ComposeKind::Box, BoxStyle{.interactive = false, .clip = true}, second);
}

void Context::end_split() {
    if (_splits_open.empty()) {
        return;
    }
    end_box(); // the second pane
    end_box(); // the split
    _splits_open.pop_back();
}

f32 Context::split_ratio(Id id) const {
    const auto it = _split_ratios.find(id.value);
    return it != _split_ratios.end() ? it->second : 0.0f;
}

// --- end_layout: scrolling and records -----------------------------------------------

// Scrolls the nearest scroll box around `target` (as of last frame) so it is in view:
// a box, or a widget that takes focus (by the box around it).
void Context::reveal(const Reveal& target) {
    auto found = _compose_records.find(target.id.value);
    if (found == _compose_records.end()) {
        if (const auto box = _focus_boxes.find(target.id.value); box != _focus_boxes.end() && box->second) {
            found = _compose_records.find(box->second.value);
        }
    }
    if (found == _compose_records.end()) {
        return;
    }
    Rectf rect = found->second.rect;
    if (target.row >= 0 && found->second.row_height > 0.0f) {
        rect = {rect.x, rect.y + static_cast<f32>(target.row) * found->second.row_height, rect.w, found->second.row_height};
    }
    for (auto it = found->second.parent ? _compose_records.find(found->second.parent.value) : _compose_records.end();
         it != _compose_records.end();
         it = it->second.parent ? _compose_records.find(it->second.parent.value) : _compose_records.end()) {
        if (!it->second.scroll) {
            continue;
        }
        ScrollRecord& scroll = _scroll_records[it->first];
        const Rectf view = it->second.rect;
        if (rect.y < view.y) {
            scroll.offset.y -= view.y - rect.y;
        } else if (rect.y + rect.h > view.y + view.h) {
            scroll.offset.y += std::min(rect.y + rect.h - view.y - view.h, rect.y - view.y);
        }
        if (rect.x < view.x) {
            scroll.offset.x -= view.x - rect.x;
        } else if (rect.x + rect.w > view.x + view.w) {
            scroll.offset.x += std::min(rect.x + rect.w - view.x - view.w, rect.x - view.x);
        }
        return; // the nearest one
    }
}

void Context::layout_compose() {
    const std::size_t count = _nodes.size();
    // One past the last descendant of each node (the arena is pre-order).
    std::vector<i32> subtree_end(count);
    for (i32 i = static_cast<i32>(count) - 1; i >= 0; --i) {
        const LayoutNode& node = _nodes[static_cast<std::size_t>(i)];
        subtree_end[static_cast<std::size_t>(i)] =
            node.children.empty() ? i + 1 : subtree_end[static_cast<std::size_t>(node.children.back())];
    }
    std::vector<char> is_bar(count, 0);
    for (const ComposeNode& c : _compose) {
        if (c.kind == ComposeKind::ScrollbarY || c.kind == ComposeKind::ScrollbarX) {
            is_bar[static_cast<std::size_t>(c.node)] = 1;
        }
    }

    // Content and viewport sizes, from this frame's solve.
    for (const ComposeNode& c : _compose) {
        if (c.kind != ComposeKind::Scroll) {
            continue;
        }
        const LayoutNode& node = _nodes[static_cast<std::size_t>(c.node)];
        Vec2f end{node.solved.x + node.style.padding.left, node.solved.y + node.style.padding.top};
        for (const i32 child : node.children) {
            const LayoutNode& n = _nodes[static_cast<std::size_t>(child)];
            if (!is_bar[static_cast<std::size_t>(child)]) {
                end.x = std::max(end.x, n.solved.x + n.solved.w + n.style.margin.right);
                end.y = std::max(end.y, n.solved.y + n.solved.h + n.style.margin.bottom);
            }
        }
        ScrollRecord& record = _scroll_records[c.id.value];
        record.content = {end.x + node.style.padding.right - node.solved.x, end.y + node.style.padding.bottom - node.solved.y};
        if (!c.scroll.horizontal) {
            record.content.x = node.solved.w;
        }
        record.viewport = {node.solved.w, node.solved.h};
        record.frame = _state.frame();
    }

    // The wheel goes to the innermost scroll box under the pointer (where it was
    // last frame) that can move that way, and that what was on top there last
    // frame is in: not one under a dialog or a menu. Shift turns a vertical
    // wheel sideways.
    const Id hot = _state.hot();
    const auto hot_record = hot ? _compose_records.find(hot.value) : _compose_records.end();
    const auto under_top = [&](const ComposeNode& c, const ComposeRecord& record) {
        if (!hot || hot == c.id || hot == make_id(c.id, "thumb_y") || hot == make_id(c.id, "thumb_x")) {
            return true;
        }
        if (hot_record == _compose_records.end()) {
            return _state.hot_z() <= record.z; // a field's, a row's: not above the box
        }
        auto it = hot_record;
        for (std::size_t guard = 0; it != _compose_records.end() && guard < 256; ++guard) {
            if (it->second.parent == c.id) {
                return true;
            }
            it = it->second.parent ? _compose_records.find(it->second.parent.value) : _compose_records.end();
        }
        return false;
    };
    Vec2f wheel{_input ? mouse_wheel_x() : 0.0f, _input ? mouse_wheel_y() : 0.0f};
    if (wheel.x == 0.0f && wheel.y != 0.0f && modifier_held(KeyModifiers::Shift)) {
        wheel = {-wheel.y, 0.0f};
    }
    const auto under_pointer = [&](i32 axis) {
        const ComposeNode* target = nullptr;
        for (const ComposeNode& c : _compose) {
            if (c.kind != ComposeKind::Scroll || !(axis == 0 ? c.scroll.vertical : c.scroll.horizontal)) {
                continue;
            }
            const auto record = _compose_records.find(c.id.value);
            const ScrollRecord& scroll = _scroll_records[c.id.value];
            if (record != _compose_records.end() && contains(record->second.visible, pointer()) &&
                along(scroll.content, axis) > along(scroll.viewport, axis) + 0.5f && under_top(c, record->second)) {
                target = &c; // pre-order: later is deeper
            }
        }
        return target;
    };
    for (i32 axis = 0; axis < 2; ++axis) {
        const f32 turn = along(wheel, axis);
        if (turn == 0.0f) {
            continue;
        }
        if (const ComposeNode* target = under_pointer(axis)) {
            // Wheel y is positive away from the player (content moves down); x positive to the right.
            along(_scroll_records[target->id.value].offset, axis) += axis == 0 ? -turn * target->scroll.wheel_step
                                                                              : turn * target->scroll.wheel_step;
        }
    }

    // PageUp and PageDown a page, Home and End to either end: the scroll box the
    // focus is in, else the one under the pointer (not while a text field
    // takes the keys).
    if (_input && !_wants_text_input_last) {
        const bool up = key_typed(Key::PageUp);
        const bool down = key_typed(Key::PageDown);
        const bool home = key_pressed(Key::Home);
        const bool end = key_pressed(Key::End);
        if (up || down || home || end) {
            const ComposeNode* target = nullptr;
            Id at = _state.focused();
            if (const auto box = _focus_boxes.find(at.value); box != _focus_boxes.end() && !_compose_records.contains(at.value)) {
                at = box->second;
            }
            for (std::size_t guard = 0; at && !target && guard < 256; ++guard) {
                const auto record = _compose_records.find(at.value);
                if (record == _compose_records.end()) {
                    break;
                }
                if (record->second.scroll) {
                    for (const ComposeNode& c : _compose) {
                        const ScrollRecord& scroll = _scroll_records[c.id.value];
                        if (c.kind == ComposeKind::Scroll && c.id == at && c.scroll.vertical &&
                            scroll.content.y > scroll.viewport.y + 0.5f) {
                            target = &c;
                        }
                    }
                }
                at = record->second.parent;
            }
            if (!target) {
                target = under_pointer(0);
            }
            if (target) {
                ScrollRecord& scroll = _scroll_records[target->id.value];
                const f32 page = std::max(scroll.viewport.y - target->scroll.wheel_step, scroll.viewport.y * 0.5f);
                scroll.offset.y = home ? 0.0f : end ? scroll.content.y : scroll.offset.y + (down ? page : -page);
            }
        }
    }

    // Clamp, then move each scroll box's content (outer boxes first, so nested
    // content is moved by both). One that sticks to its end and was there
    // stays there, unless it was scrolled this frame.
    for (const ComposeNode& c : _compose) {
        if (c.kind != ComposeKind::Scroll) {
            continue;
        }
        ScrollRecord& record = _scroll_records[c.id.value];
        const f32 end_y = std::max(0.0f, record.content.y - record.viewport.y);
        if (c.scroll.stick_to_end && record.at_end && record.offset.y == record.laid_out.y) {
            record.offset.y = end_y;
        }
        record.offset.x = std::clamp(record.offset.x, 0.0f, std::max(0.0f, record.content.x - record.viewport.x));
        record.offset.y = std::clamp(record.offset.y, 0.0f, end_y);
        record.at_end = record.offset.y >= end_y - 0.5f;
        record.laid_out = record.offset;
        if (record.offset.x == 0.0f && record.offset.y == 0.0f) {
            continue;
        }
        for (i32 k = c.node + 1; k < subtree_end[static_cast<std::size_t>(c.node)]; ++k) {
            if (!is_bar[static_cast<std::size_t>(k)] || _nodes[static_cast<std::size_t>(k)].parent != c.node) {
                _nodes[static_cast<std::size_t>(k)].solved.x -= record.offset.x;
                _nodes[static_cast<std::size_t>(k)].solved.y -= record.offset.y;
            }
        }
    }
    // A floating box hanging off scrolled content moved with it: keep it in the root.
    place_floating(_nodes, _root, _layout_scratch);
}

void Context::record_compose(const std::vector<i32>& clip_of, const std::vector<Rectf>& visible_clips) {
    const u64 frame = _state.frame();
    std::vector<i32> compose_of(_nodes.size(), -1);
    for (std::size_t i = 0; i < _compose.size(); ++i) {
        compose_of[static_cast<std::size_t>(_compose[i].node)] = static_cast<i32>(i);
    }
    // Window chrome: the drag boxes, less the interactive boxes inside them.
    _chrome.drag.clear();
    _chrome.exclude.clear();
    std::vector<char> in_drag(_compose.size(), 0);
    for (const ComposeNode& c : _compose) {
        if (!c.id || c.kind == ComposeKind::ScrollbarY || c.kind == ComposeKind::ScrollbarX) {
            continue;
        }
        const LayoutNode& node = _nodes[static_cast<std::size_t>(c.node)];
        const i32 clip = clip_of[static_cast<std::size_t>(c.node)];
        Id parent{};
        for (i32 p = node.parent; p >= 0; p = _nodes[static_cast<std::size_t>(p)].parent) {
            const i32 owner = compose_of[static_cast<std::size_t>(p)];
            if (owner >= 0 && _compose[static_cast<std::size_t>(owner)].id) {
                parent = _compose[static_cast<std::size_t>(owner)].id;
                break;
            }
        }
        ComposeRecord& record = _compose_records[c.id.value];
        record.rect = node.solved;
        record.visible = clip >= 0 ? intersect(node.solved, visible_clips[static_cast<std::size_t>(clip)]) : node.solved;
        const std::size_t self = static_cast<std::size_t>(compose_of[static_cast<std::size_t>(c.node)]);
        bool parent_drags = false;
        for (i32 p = node.parent; p >= 0 && !parent_drags; p = _nodes[static_cast<std::size_t>(p)].parent) {
            const i32 owner = compose_of[static_cast<std::size_t>(p)];
            parent_drags = owner >= 0 && in_drag[static_cast<std::size_t>(owner)];
        }
        in_drag[self] = c.window_drag || parent_drags;
        if (c.window_drag) {
            _chrome.drag.push_back(record.visible);
        } else if (parent_drags && c.interactive) {
            _chrome.exclude.push_back(record.visible);
        }
        record.parent = parent;
        record.scroll = c.kind == ComposeKind::Scroll;
        record.z = c.z;
        record.row_height = c.row_height;
        record.key_context = c.key_context;
        record.frame = frame;
    }
    if (frame % stale_frames == 0) {
        std::erase_if(_compose_records, [frame](const auto& kv) { return kv.second.frame + stale_frames < frame; });
        std::erase_if(_scroll_records, [frame](const auto& kv) { return kv.second.frame + stale_frames < frame; });
    }
}

// --- focus and key bindings ----------------------------------------------------------

void Context::register_focusable(Id id, i32 tab_index) {
    add_focusable(id, tab_index, false);
}

void Context::add_focusable(Id id, i32 tab_index, bool box_entry) {
    if (!id) {
        return;
    }
    const bool running = _running_node >= 0 && _running_node < static_cast<i32>(_node_owner.size());
    _focus_order.push_back({
        .id = id,
        .box = running ? _node_owner[static_cast<std::size_t>(_running_node)] : !_compose_ids.empty() ? _compose_ids.back() : Id{},
        .tab_index = tab_index,
        // A widget registers as end_layout() runs it, after every box: its node
        // places it among them. A box registers before its node is added.
        .order = _running_node >= 0 ? _running_node : static_cast<i32>(_nodes.size()),
        .box_entry = box_entry,
    });
}

void Context::request_focus_move(Id from, i32 direction) {
    _focus_move = std::pair{from, direction < 0 ? -1 : 1};
}

// At end(): Tab moves focus along this frame's order; the new focus is scrolled
// into view next frame.
void Context::finish_focus() {
    std::stable_sort(_focus_order.begin(), _focus_order.end(), [](const FocusEntry& a, const FocusEntry& b) {
        return a.tab_index != b.tab_index ? a.tab_index < b.tab_index : a.order < b.order;
    });
    // A focus trap built this frame (a dialog): Tab goes round what is in it,
    // and focus left behind it is dropped.
    if (_focus_trap) {
        const auto in_trap = [&](Id at) {
            for (std::size_t guard = 0; at && guard < 256; ++guard) {
                if (at == _focus_trap) {
                    return true;
                }
                const auto record = _compose_records.find(at.value);
                if (record == _compose_records.end()) {
                    return false;
                }
                at = record->second.parent;
            }
            return false;
        };
        std::erase_if(_focus_order, [&](const FocusEntry& e) { return e.id != _focus_trap && !in_trap(e.box); });
        const Id focused = _state.focused();
        if (focused) {
            const auto entry = std::ranges::find(_focus_order, focused, &FocusEntry::id);
            const auto box = _focus_boxes.find(focused.value);
            const bool inside = entry != _focus_order.end() || in_trap(focused) ||
                                (box != _focus_boxes.end() && in_trap(box->second));
            if (!inside) {
                _state.clear_focused();
            }
        }
    }
    const bool opted_in = std::ranges::any_of(_focus_order, [](const FocusEntry& e) { return e.box_entry; });
    const auto position = [&](Id id) {
        const auto it = std::ranges::find_if(_focus_order, [&](const FocusEntry& e) { return e.id == id; });
        return it != _focus_order.end() ? static_cast<i32>(it - _focus_order.begin()) : -1;
    };
    if (!_focus_move && _input && !_wants_text_input && key_typed(Key::Tab) &&
        (opted_in || _focus_trap || position(_state.focused()) >= 0)) {
        _focus_move = std::pair{_state.focused(), modifier_held(KeyModifiers::Shift) ? -1 : 1};
    }
    _focus_moved = false;
    if (_focus_move && !_focus_order.empty()) {
        const auto [from, direction] = *_focus_move;
        const i32 n = static_cast<i32>(_focus_order.size());
        const i32 at = position(from);
        const i32 next = at < 0 ? (direction > 0 ? 0 : n - 1) : ((at + direction) % n + n) % n;
        const Id to = _focus_order[static_cast<std::size_t>(next)].id;
        if (to != from) { // alone in the order, Tab just leaves it
            _state.set_focused(to); // by the keys: its ring shows
            reveal({to, -1});
            _focus_moved = true;
        }
    }
    _focus_move.reset();

    _focus_boxes.clear();
    for (const FocusEntry& e : _focus_order) {
        _focus_boxes[e.id.value] = e.box;
    }
    _focus_order.clear();
}

void Context::bind(KeyBinding binding) {
    _bindings.push_back(std::move(binding));
}

bool Context::action_triggered(std::string_view action) const {
    return std::ranges::find(_actions, action) != _actions.end();
}

// At begin(): which bindings the keys pressed fire, from the focused box out to
// the root (as of last frame). For each key, the innermost matching context wins.
void Context::dispatch_bindings() {
    _actions.clear();
    if (_bindings.empty() || !_input) {
        return;
    }
    std::vector<std::string_view> contexts; // innermost first
    Id box = _state.focused();
    if (const auto it = _focus_boxes.find(box.value); it != _focus_boxes.end() && !_compose_records.contains(box.value)) {
        box = it->second; // a text field: start from the box around it
    }
    for (std::size_t guard = 0; box && guard < 256; ++guard) {
        const auto record = _compose_records.find(box.value);
        if (record == _compose_records.end()) {
            break;
        }
        if (!record->second.key_context.empty()) {
            contexts.push_back(record->second.key_context);
        }
        box = record->second.parent;
    }
    const auto depth = [&](const std::string& context) -> i32 {
        if (context.empty()) {
            return static_cast<i32>(contexts.size()); // global: outermost
        }
        const auto it = std::ranges::find(contexts, std::string_view(context));
        return it != contexts.end() ? static_cast<i32>(it - contexts.begin()) : -1;
    };

    std::vector<std::pair<const KeyBinding*, i32>> fired;
    for (const KeyBinding& binding : _bindings) {
        if (!key_pressed(binding.key) || !exact_modifiers(*this, binding.modifiers)) {
            continue;
        }
        const bool chord = has(binding.modifiers, KeyModifiers::Ctrl) || has(binding.modifiers, KeyModifiers::Alt);
        if (_wants_text_input_last && !chord) {
            continue; // typing, not a command
        }
        const i32 d = depth(binding.context);
        if (d < 0) {
            continue;
        }
        const auto same_key = std::ranges::find_if(fired, [&](const auto& f) {
            return f.first->key == binding.key && f.first->modifiers == binding.modifiers;
        });
        if (same_key == fired.end()) {
            fired.emplace_back(&binding, d);
        } else if (d < same_key->second) {
            *same_key = {&binding, d};
        }
    }
    for (const auto& [binding, d] : fired) {
        _actions.push_back(binding->action);
    }
}

} // namespace kin::ui2
