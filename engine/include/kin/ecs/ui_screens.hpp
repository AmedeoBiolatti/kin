#pragma once

// UI screens (.kinui, kin/ui2/screen.hpp) shown by a scene: an entity with a
// UiScreenComponent shows its screen over the world, in the look of the
// .kintheme the screen names. SceneScreens reads the files (again as they
// change), draws the visible ones in their order, and is how the game
// reaches their elements by id:
//
//     kin::SceneScreens screens{kin::content_root()};
//     // each frame, after the world is drawn, inside ui.begin()/end():
//     screens.draw(world, ui, renderer, {0, 0, width, height}, assets);
//     // in update:
//     screens.set_text("score", std::to_string(score));
//     if (screens.clicked("pause")) paused = true;
//     screens.clear_events(); // once the step has seen them
//
// An element id is looked for in each screen shown, in their order (the
// first that has it); a screen may be asked for by its file's name too
// (screen("hud") for screens/hud.kinui).

#include <kin/ecs/component.hpp>
#include <kin/ecs/world.hpp>
#include <kin/ui2/screen.hpp>
#include <kin/ui2/theme_file.hpp>

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kin {

class Assets2D;
class Renderer2D;
class SceneDialogue;

struct UiScreenComponent {
    ComponentAssetRef asset{}; // a .kinui file of the content
    bool visible = true;
    i32 order = 0; // higher draws over lower
};

class SceneScreens {
public:
    explicit SceneScreens(std::filesystem::path content, i32 check_frames = 30);
    ~SceneScreens();
    SceneScreens(const SceneScreens&) = delete;
    SceneScreens& operator=(const SceneScreens&) = delete;

    // The world's shown screens drawn over `bounds` (points) with `ui` (between
    // its begin() and end()), their images from `assets` (none: not drawn).
    // Files changed are read again every so many draws. The context's theme
    // is left as it was.
    void draw(EcsWorld& world, ui2::Context& ui, Renderer2D& renderer, Rectf bounds, Assets2D* assets = nullptr);

    // A screen shown at the last draw, by its file's name ("hud") or path.
    ui2::UiScreen* screen(std::string_view name);
    // By element id, in the first shown screen that has it.
    bool clicked(std::string_view id) const;
    bool changed(std::string_view id) const;
    f32 value(std::string_view id) const;
    bool checked(std::string_view id) const;
    std::string text(std::string_view id) const;
    void set_text(std::string_view id, std::string text);
    void set_value(std::string_view id, f32 value);
    void set_checked(std::string_view id, bool checked);
    void set_visible(std::string_view id, bool visible);
    void set_image(std::string_view id, std::string path);
    void set_enabled(std::string_view id, bool enabled);
    // Every screen's clicks and changes forgotten (the game has seen them).
    void clear_events();

    // The dialogue their `dialogue` elements show, and which the player's
    // clicks on them move on (null: none).
    void set_dialogue(SceneDialogue* dialogue) { _dialogue = dialogue; }

    // Screens and themes that could not be read, by path, with why.
    const std::map<std::string, std::string>& problems() const { return _problems; }

private:
    struct Read;
    struct Theme;
    Read* read(const std::string& path);
    Theme* theme(const std::string& path, Renderer2D& renderer, Assets2D* assets);
    ui2::UiScreen* holding(std::string_view id);
    const ui2::UiScreen* holding(std::string_view id) const;

    std::filesystem::path _content;
    i32 _check_frames = 30;
    i32 _frames = 0;
    std::map<std::string, std::unique_ptr<Read>, std::less<>> _screens;
    std::map<std::string, std::unique_ptr<Theme>, std::less<>> _themes;
    std::vector<ui2::UiScreen*> _shown; // at the last draw, in order
    // What the game set for ids no screen shown had yet: given to the first that has them.
    struct Pending {
        std::optional<std::string> text;
        std::optional<f32> value;
        std::optional<bool> checked;
        std::optional<bool> visible;
        std::optional<std::string> image;
        std::optional<bool> enabled;
    };
    std::map<std::string, Pending, std::less<>> _pending;
    std::map<std::string, std::string> _problems;
    SceneDialogue* _dialogue = nullptr;
};

} // namespace kin
