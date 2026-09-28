#pragma once

#include <kin/scene/scene.hpp>
#include <kin/ui2/context.hpp>
#include <kin/ui2/theme.hpp>

#include <functional>
#include <string>
#include <vector>

namespace kin::ui2 {

struct MenuSceneItem {
    std::string label;
    bool enabled = true;
    std::function<void(SceneContext&)> action;
};

struct MenuSceneLayout {
    f32 title_y = 112.0f;
    Vec2f title_size{};
    f32 menu_width = 220.0f;
    f32 row_height = 40.0f;
    f32 row_spacing = 12.0f;
    UiPadding padding{16.0f, 12.0f, 16.0f, 12.0f};
    UiAnchor anchor = UiAnchor::Center;
    Vec2f offset{};
    f32 min_height = 0.0f;
};

struct MenuSceneConfig {
    std::string name = "Ui2MenuScene";
    std::string title;
    std::string hint;
    std::vector<MenuSceneItem> items;
    Vec2f size{640.0f, 480.0f};
    f32 margin = 0.0f;
    MenuSceneLayout layout{};
    PalettePreset theme = PalettePreset::Default;
    f32 title_scale = 8.0f;
    f32 hint_scale = 3.0f;
    f32 hint_y = -1.0f;
    Color overlay = Color::rgba(7, 9, 14, 208);
    std::function<void(SceneContext&)> render_background;
    std::function<void(SceneContext&)> cancel;
};

class MenuScene final : public Scene {
public:
    explicit MenuScene(MenuSceneConfig config);

    std::string_view name() const override { return _config.name; }

    void update(SceneContext& ctx) override;
    void render(SceneContext& ctx) override;
    void collect_actions(InputActionContext& context) const override;

private:
    struct MenuLayout {
        Rectf title{};
        Rectf panel{};
        Rectf items{};
    };

    MenuLayout layout() const;
    MenuList menu_list() const;

    MenuSceneConfig _config;
    Context _ui;
    i32 _selected = 0;
    f32 _menu_scroll_offset = 0.0f;
    // A mouse click whose release lands on a frame that ran zero fixed-update steps is
    // resolved by render() (the only pass that runs that frame); render() can't safely
    // fire scene-changing actions, so it parks the index here for the next update() to
    // act on. -1 means nothing pending. Keeps menus single-click at refresh > sim rate.
    i32 _render_activated = -1;
};

} // namespace kin::ui2
