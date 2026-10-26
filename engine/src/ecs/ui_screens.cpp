#include <kin/ecs/ui_screens.hpp>

#include <kin/assets/content.hpp>
#include <kin/ecs/assets2d.hpp>
#include <kin/platform/log.hpp>
#include <kin/renderer/renderer2d.hpp>

#include <algorithm>
#include <system_error>

namespace kin {
namespace {

using FileTime = std::optional<std::filesystem::file_time_type>;

FileTime time_of(const std::filesystem::path& file) {
    std::error_code error;
    const auto time = std::filesystem::last_write_time(file, error);
    return error ? std::nullopt : FileTime(time);
}

void tell(std::map<std::string, std::string>& problems, const std::string& path, std::string why) {
    const auto [found, fresh] = problems.insert_or_assign(path, std::move(why));
    if (fresh) {
        KIN_LOG_WARN_F("ui", "ui file could not be read", (LogFields{{.name = "path", .value = path}, {.name = "error", .value = found->second}}));
    }
}

} // namespace

struct SceneScreens::Read {
    ui2::UiScreen screen;
    FileTime time;
    bool read = false; // a version read (else nothing to draw)
};

struct SceneScreens::Theme {
    ui2::ThemeFile file;
    std::vector<std::pair<std::filesystem::path, FileTime>> files; // itself, its fonts and images
    bool built = false;
    bool read = false;
};

SceneScreens::SceneScreens(std::filesystem::path content, i32 check_frames)
    : _content(std::move(content)),
      _check_frames(check_frames) {}

SceneScreens::~SceneScreens() = default;

SceneScreens::Read* SceneScreens::read(const std::string& path) {
    auto found = _screens.find(path);
    if (found == _screens.end()) {
        found = _screens.emplace(path, std::make_unique<Read>()).first;
        found->second->time = std::nullopt;
        found->second->screen.set_seed(std::hash<std::string>{}(path));
    }
    Read& r = *found->second;
    const std::filesystem::path file = _content / path;
    const FileTime now = time_of(file);
    if (r.read && now == r.time) {
        return &r;
    }
    r.time = now;
    const std::optional<std::string> text = read_content_file(file);
    ui2::ScreenDocument document;
    std::string error;
    if (!text) {
        tell(_problems, path, "it could not be read");
    } else if (!ui2::parse_screen(*text, document, error)) {
        tell(_problems, path, error);
    } else {
        r.screen.set_document(std::move(document)); // what the game set stays
        r.read = true;
        _problems.erase(path);
    }
    return r.read ? &r : nullptr;
}

SceneScreens::Theme* SceneScreens::theme(const std::string& path, Renderer2D& renderer, Assets2D* assets) {
    auto found = _themes.find(path);
    if (found == _themes.end()) {
        found = _themes.emplace(path, std::make_unique<Theme>()).first;
    }
    Theme& t = *found->second;
    const bool stale = !t.read || std::ranges::any_of(t.files, [](const auto& file) { return time_of(file.first) != file.second; });
    if (!stale) {
        return t.read ? &t : nullptr;
    }
    const std::filesystem::path file = _content / path;
    t.files = {{file, time_of(file)}};
    std::vector<std::string> errors;
    ui2::ThemeFile made;
    if (!ui2::load_theme_file(file, made, errors)) {
        std::string why;
        for (const std::string& e : errors) {
            why += (why.empty() ? "" : "; ") + e;
        }
        tell(_problems, path, why);
        t.read = t.read && true; // keep the last good one, if any
        return t.read ? &t : nullptr;
    }
    std::vector<std::string> problems;
    const f32 scale = renderer.content_scale() > 0.0f ? renderer.content_scale() : 1.0f;
    std::function<Texture(std::string_view)> image;
    if (assets) {
        image = [assets](std::string_view p) { return assets->texture(p); };
    }
    made.build(renderer, _content, problems, scale, image);
    for (const std::string& font : {made.font, made.bold_font}) {
        if (!font.empty()) {
            t.files.emplace_back(_content / font, time_of(_content / font));
        }
    }
    for (const auto& [slot, frame] : made.skin) {
        if (!frame.image.empty()) {
            t.files.emplace_back(_content / frame.image, time_of(_content / frame.image));
        }
    }
    if (!problems.empty()) {
        std::string why;
        for (const std::string& p : problems) {
            why += (why.empty() ? "" : "; ") + p;
        }
        tell(_problems, path, why);
    } else {
        _problems.erase(path);
    }
    t.file = std::move(made);
    t.read = true;
    return &t;
}

void SceneScreens::draw(EcsWorld& world, ui2::Context& ui, Renderer2D& renderer, Rectf bounds, Assets2D* assets) {
    // Which, in order: by their order, then as the world has them.
    struct Shown {
        i32 order = 0;
        std::size_t index = 0;
        std::string path;
    };
    std::vector<Shown> shown;
    world.raw().each([&](const UiScreenComponent& screen) {
        if (screen.visible && !screen.asset.id.empty()) {
            shown.push_back({screen.order, shown.size(), screen.asset.id});
        }
    });
    std::ranges::stable_sort(shown, {}, &Shown::order);
    const bool check = _check_frames > 0 && ++_frames >= _check_frames;
    if (check) {
        _frames = 0;
    }
    _shown.clear();
    const ui2::Theme was = ui.theme();
    ui2::ScreenAssets with;
    if (assets) {
        with.texture = [assets](std::string_view p) { return assets->texture(p); };
    }
    for (const Shown& item : shown) {
        Read* r = nullptr;
        if (const auto found = _screens.find(item.path); found != _screens.end() && found->second->read && !check) {
            r = found->second.get();
        } else {
            r = read(item.path);
        }
        if (!r) {
            continue;
        }
        // What the game set for its ids before it was shown.
        for (auto pending = _pending.begin(); pending != _pending.end();) {
            if (!r->screen.has(pending->first)) {
                ++pending;
                continue;
            }
            const Pending& p = pending->second;
            if (p.text) r->screen.set_text(pending->first, *p.text);
            if (p.value) r->screen.set_value(pending->first, *p.value);
            if (p.checked) r->screen.set_checked(pending->first, *p.checked);
            if (p.visible) r->screen.set_visible(pending->first, *p.visible);
            if (p.image) r->screen.set_image(pending->first, *p.image);
            if (p.enabled) r->screen.set_enabled(pending->first, *p.enabled);
            pending = _pending.erase(pending);
        }
        const std::string& theme_path = r->screen.document().theme;
        Theme* t = nullptr;
        if (!theme_path.empty()) {
            if (const auto found = _themes.find(theme_path); found != _themes.end() && found->second->read && !check) {
                t = found->second.get();
            } else {
                t = theme(theme_path, renderer, assets);
            }
        }
        ui.set_theme(t ? t->file.theme : was);
        r->screen.draw(ui, bounds, with);
        _shown.push_back(&r->screen);
    }
    ui.set_theme(was);
}

ui2::UiScreen* SceneScreens::screen(std::string_view name) {
    for (auto& [path, r] : _screens) {
        if (!r->read || std::ranges::find(_shown, &r->screen) == _shown.end()) {
            continue;
        }
        const std::filesystem::path file{path};
        std::string stem = file.filename().string();
        if (const std::size_t dot = stem.find('.'); dot != std::string::npos) {
            stem.erase(dot);
        }
        if (path == name || stem == name) {
            return &r->screen;
        }
    }
    return nullptr;
}

ui2::UiScreen* SceneScreens::holding(std::string_view id) {
    for (ui2::UiScreen* screen : _shown) {
        if (screen->has(id)) {
            return screen;
        }
    }
    return nullptr;
}

const ui2::UiScreen* SceneScreens::holding(std::string_view id) const {
    for (const ui2::UiScreen* screen : _shown) {
        if (screen->has(id)) {
            return screen;
        }
    }
    return nullptr;
}

bool SceneScreens::clicked(std::string_view id) const {
    const ui2::UiScreen* s = holding(id);
    return s && s->clicked(id);
}

bool SceneScreens::changed(std::string_view id) const {
    const ui2::UiScreen* s = holding(id);
    return s && s->changed(id);
}

f32 SceneScreens::value(std::string_view id) const {
    const ui2::UiScreen* s = holding(id);
    return s ? s->value(id) : 0.0f;
}

bool SceneScreens::checked(std::string_view id) const {
    const ui2::UiScreen* s = holding(id);
    return s && s->checked(id);
}

std::string SceneScreens::text(std::string_view id) const {
    const ui2::UiScreen* s = holding(id);
    return s ? s->text(id) : std::string();
}

void SceneScreens::set_text(std::string_view id, std::string text) {
    if (ui2::UiScreen* s = holding(id)) {
        s->set_text(id, std::move(text));
    } else {
        _pending[std::string(id)].text = std::move(text);
    }
}

void SceneScreens::set_value(std::string_view id, f32 value) {
    if (ui2::UiScreen* s = holding(id)) {
        s->set_value(id, value);
    } else {
        _pending[std::string(id)].value = value;
    }
}

void SceneScreens::set_checked(std::string_view id, bool checked) {
    if (ui2::UiScreen* s = holding(id)) {
        s->set_checked(id, checked);
    } else {
        _pending[std::string(id)].checked = checked;
    }
}

void SceneScreens::set_visible(std::string_view id, bool visible) {
    if (ui2::UiScreen* s = holding(id)) {
        s->set_visible(id, visible);
    } else {
        _pending[std::string(id)].visible = visible;
    }
}

void SceneScreens::set_image(std::string_view id, std::string path) {
    if (ui2::UiScreen* s = holding(id)) {
        s->set_image(id, std::move(path));
    } else {
        _pending[std::string(id)].image = std::move(path);
    }
}

void SceneScreens::set_enabled(std::string_view id, bool enabled) {
    if (ui2::UiScreen* s = holding(id)) {
        s->set_enabled(id, enabled);
    } else {
        _pending[std::string(id)].enabled = enabled;
    }
}

void SceneScreens::clear_events() {
    for (auto& [path, r] : _screens) {
        r->screen.clear_events();
    }
}

} // namespace kin
