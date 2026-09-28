#include <kin/ui2/menu_scene.hpp>

#include <algorithm>
#include <utility>

namespace kin::ui2 {
namespace {

Rectf safe_bounds(Vec2f size, f32 margin) {
    return {
        margin,
        margin,
        std::max(0.0f, size.x - margin * 2.0f),
        std::max(0.0f, size.y - margin * 2.0f),
    };
}

Rectf anchored_rect(Rectf safe, UiAnchor anchor, Vec2f size, Vec2f offset) {
    f32 x = safe.x;
    f32 y = safe.y;

    switch (anchor) {
    case UiAnchor::TopLeft:
    case UiAnchor::CenterLeft:
    case UiAnchor::BottomLeft:
        x = safe.x;
        break;
    case UiAnchor::TopCenter:
    case UiAnchor::Center:
    case UiAnchor::BottomCenter:
        x = safe.x + (safe.w - size.x) * 0.5f;
        break;
    case UiAnchor::TopRight:
    case UiAnchor::CenterRight:
    case UiAnchor::BottomRight:
        x = safe.x + safe.w - size.x;
        break;
    }

    switch (anchor) {
    case UiAnchor::TopLeft:
    case UiAnchor::TopCenter:
    case UiAnchor::TopRight:
        y = safe.y;
        break;
    case UiAnchor::CenterLeft:
    case UiAnchor::Center:
    case UiAnchor::CenterRight:
        y = safe.y + (safe.h - size.y) * 0.5f;
        break;
    case UiAnchor::BottomLeft:
    case UiAnchor::BottomCenter:
    case UiAnchor::BottomRight:
        y = safe.y + safe.h - size.y;
        break;
    }

    return {x + offset.x, y + offset.y, size.x, size.y};
}

} // namespace

MenuScene::MenuScene(MenuSceneConfig config)
    : _config(std::move(config)) {
}

void MenuScene::update(SceneContext& ctx) {
    if (ctx.input.pressed("quit")) {
        if (_config.cancel) {
            _config.cancel(ctx);
        }
        return;
    }

    // A click resolved by render() on a 0-step frame parks its index here (render()
    // must not fire scene-changing actions). Act on it now, in update().
    if (_render_activated >= 0) {
        const i32 pending = _render_activated;
        _render_activated = -1;
        if (pending < static_cast<i32>(_config.items.size())) {
            MenuSceneItem& item = _config.items[static_cast<std::size_t>(pending)];
            if (item.enabled && item.action) {
                item.action(ctx);
                return;
            }
        }
    }

    _ui.set_theme(game_theme(_config.theme));
    _ui.begin(ctx.input, ctx.renderer);

    MenuList menu = menu_list();
    run(_ui, menu);
    _selected = menu.selected;
    _menu_scroll_offset = menu.scroll_offset;

    _ui.end();

    // The menu navigates/activates here (update). render() redraws the menu and
    // would re-read the same per-frame edges (keyboard frame_pressed AND mouse
    // clicks) a second time — double nav / double activation. Consume this frame's
    // edges now so render() draws at the resolved selection without re-processing.
    // Other scenes that read input in render() (text fields, the widget zoo) are
    // unaffected: only THIS scene's input was consumed. (The input loop no longer
    // clears these globally — that dropped ~half of render-time clicks/keystrokes.)
    ctx.input.consume_frame_edges();

    if (menu.activated >= 0 && menu.activated < static_cast<i32>(_config.items.size())) {
        MenuSceneItem& item = _config.items[static_cast<std::size_t>(menu.activated)];
        if (item.enabled && item.action) {
            item.action(ctx);
        }
    }
}

void MenuScene::render(SceneContext& ctx) {
    if (_config.render_background) {
        _config.render_background(ctx);
    }

    const Theme theme = game_theme(_config.theme);
    const MenuLayout menu_layout = layout();
    ctx.renderer.fill_rect({0.0f, 0.0f, _config.size.x, _config.size.y}, _config.overlay);

    _ui.set_theme(theme);
    _ui.begin(ctx.input, ctx.renderer);

    if (!_config.title.empty()) {
        Label title{
            .bounds = menu_layout.title,
            .text = _config.title,
            .text_style = {.scale = _config.title_scale, .color = theme.palette.accent},
            .horizontal = UiAlign::Center,
            .vertical = UiAlign::Center,
        };
        run(_ui, title);
    }

    MenuList menu = menu_list();
    run(_ui, menu);
    _selected = menu.selected;
    _menu_scroll_offset = menu.scroll_offset;
    // On a 0-step frame update() did not run, so this render pass is the only place the
    // click is observed. Park the activation for the next update() to fire (see update()).
    if (menu.activated >= 0) {
        _render_activated = menu.activated;
    }

    if (!_config.hint.empty()) {
        const f32 y = _config.hint_y >= 0.0f ? _config.hint_y : _config.size.y - 69.5f;
        Label hint{
            .bounds = {0.0f, y - 16.0f, _config.size.x, 32.0f},
            .text = _config.hint,
            .text_style = {.scale = _config.hint_scale, .color = theme.palette.text_muted},
            .horizontal = UiAlign::Center,
            .vertical = UiAlign::Center,
        };
        run(_ui, hint);
    }

    _ui.end();
}

void MenuScene::collect_actions(InputActionContext& context) const {
    context.add("menu_up", "Move selection up");
    context.add("menu_down", "Move selection down");
    context.add("accept", "Choose item");
    context.add("quit", "Cancel menu");
}

MenuScene::MenuLayout MenuScene::layout() const {
    const i32 rows = static_cast<i32>(_config.items.size());
    const f32 content_height = rows > 0
                                   ? static_cast<f32>(rows) * _config.layout.row_height +
                                         static_cast<f32>(std::max(0, rows - 1)) * _config.layout.row_spacing
                                   : 0.0f;
    const Rectf safe = safe_bounds(_config.size, _config.margin);
    const f32 natural_panel_height =
        std::max(_config.layout.min_height, content_height + _config.layout.padding.top + _config.layout.padding.bottom);
    const Vec2f panel_size{
        _config.layout.menu_width,
        std::min(natural_panel_height, safe.h),
    };
    const Rectf panel = anchored_rect(safe, _config.layout.anchor, panel_size, _config.layout.offset);
    const Rectf items{
        panel.x + _config.layout.padding.left,
        panel.y + _config.layout.padding.top,
        std::max(0.0f, panel.w - _config.layout.padding.left - _config.layout.padding.right),
        std::max(0.0f, panel.h - _config.layout.padding.top - _config.layout.padding.bottom),
    };
    const Vec2f title_size{
        _config.layout.title_size.x > 0.0f ? _config.layout.title_size.x : _config.size.x,
        _config.layout.title_size.y > 0.0f ? _config.layout.title_size.y : 80.0f,
    };
    return {
        .title = {
            (_config.size.x - title_size.x) * 0.5f,
            _config.layout.title_y,
            title_size.x,
            title_size.y,
        },
        .panel = panel,
        .items = items,
    };
}

MenuList MenuScene::menu_list() const {
    std::vector<MenuListItem> items;
    items.reserve(_config.items.size());
    for (const MenuSceneItem& item : _config.items) {
        items.push_back({
            .label = item.label,
            .enabled = item.enabled,
        });
    }

    const MenuLayout menu_layout = layout();
    return {
        .id = make_id("menu"),
        .bounds = menu_layout.items,
        .items = std::move(items),
        .selected = std::clamp(_selected, 0, std::max(0, static_cast<i32>(_config.items.size()) - 1)),
        .row_height = _config.layout.row_height,
        .row_spacing = _config.layout.row_spacing,
        .scrollable = true,
        .scroll_offset = _menu_scroll_offset,
    };
}

} // namespace kin::ui2
