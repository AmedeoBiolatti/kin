#include <kin/ui2/screen.hpp>

#include <kin/core/json.hpp>
#include <kin/core/json_value.hpp>
#include <kin/l10n/localization.hpp>
#include <kin/ui2/theme.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace kin::ui2 {
namespace {

constexpr std::array kind_names{"box", "panel", "label", "button", "image", "progress", "toggle", "slider", "text_input", "spacer"};

struct NamedAnchor {
    std::string_view name;
    UiAnchor anchor;
};
constexpr std::array anchors{
    NamedAnchor{"top_left", UiAnchor::TopLeft},       NamedAnchor{"top", UiAnchor::TopCenter},
    NamedAnchor{"top_right", UiAnchor::TopRight},     NamedAnchor{"left", UiAnchor::CenterLeft},
    NamedAnchor{"center", UiAnchor::Center},          NamedAnchor{"right", UiAnchor::CenterRight},
    NamedAnchor{"bottom_left", UiAnchor::BottomLeft}, NamedAnchor{"bottom", UiAnchor::BottomCenter},
    NamedAnchor{"bottom_right", UiAnchor::BottomRight},
};

std::string_view align_name(UiAlign align) {
    switch (align) {
    case UiAlign::Start: return "start";
    case UiAlign::Center: return "center";
    case UiAlign::End: return "end";
    case UiAlign::Stretch: return "stretch";
    }
    return "start";
}

std::optional<UiAlign> align_named(std::string_view name) {
    for (const UiAlign align : {UiAlign::Start, UiAlign::Center, UiAlign::End, UiAlign::Stretch}) {
        if (align_name(align) == name) {
            return align;
        }
    }
    return std::nullopt;
}

std::optional<f32> number(std::string_view text) {
    f32 value = 0.0f;
    const auto [end, failed] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (failed != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::string number_text(f32 value) {
    char text[32];
    std::snprintf(text, sizeof(text), "%g", static_cast<double>(value));
    return text;
}

// The theme's surface a box names.
const SurfaceStyle* theme_surface(const Theme& theme, std::string_view name) {
    if (name == "panel") return &theme.panel_surface;
    if (name == "card") return &theme.card_surface;
    if (name == "header") return &theme.header_surface;
    if (name == "subtle") return &theme.subtle_surface;
    if (name == "overlay") return &theme.overlay_surface;
    if (name == "popup") return &theme.popup_surface;
    if (name == "tooltip") return &theme.tooltip_surface;
    if (name == "menu") return &theme.menu_surface;
    if (name == "row") return &theme.row_surface;
    if (name == "selected") return &theme.selected_surface;
    return nullptr;
}

const TextStyle& theme_text(const Theme& theme, std::string_view name) {
    if (name == "title") return theme.title_text;
    if (name == "small") return theme.small_text;
    if (name == "muted") return theme.muted_text;
    if (name == "emphasis") return theme.emphasis_text;
    if (name == "danger") return theme.danger_text;
    return theme.body_text;
}

// --- reading -----------------------------------------------------------------------------------

struct Reader {
    std::string error;

    bool fail(const std::string& where, std::string why) {
        if (error.empty()) {
            error = (where.empty() ? std::string("the root") : "element " + where) + ": " + std::move(why);
        }
        return false;
    }

    std::optional<SizeAxis> size(const JsonValue& value) {
        if (value.is_number()) {
            return fixed(static_cast<f32>(value.as_number()));
        }
        return screen_size_from_text(value.as_string());
    }

    std::optional<Color> color(const JsonValue& value) {
        if (!value.is_object()) {
            return std::nullopt;
        }
        const auto channel = [&](std::string_view name, i64 fallback) { return static_cast<u8>(std::clamp<i64>(value.int_at(name, fallback), 0, 255)); };
        return Color::rgba(channel("r", 255), channel("g", 255), channel("b", 255), channel("a", 255));
    }

    bool element(const JsonValue& json, ScreenElement& out, const std::string& where) {
        if (!json.is_object()) {
            return fail(where, "not an object");
        }
        out.id = json.string_at("id");
        const std::string label = out.id.empty() ? where : "'" + out.id + "'";
        const std::optional<ScreenKind> kind = screen_kind_named(json.string_at("kind", "box"));
        if (!kind) {
            return fail(label, "no kind '" + json.string_at("kind") + "' (box, panel, label, button, image, progress, toggle, slider, "
                               "text_input, spacer)");
        }
        out.kind = *kind;
        for (const auto& [key, value] : json.members()) {
            if (key == "id" || key == "kind" || key == "children") {
                continue;
            }
            if (key == "width" || key == "height") {
                const std::optional<SizeAxis> size_value = size(value);
                if (!size_value) {
                    return fail(label, key + ": fit, grow, a number or a percentage (\"50%\")");
                }
                (key == "width" ? out.width : out.height) = *size_value;
            } else if (key == "anchor") {
                const std::optional<UiAnchor> anchor = screen_anchor_named(value.as_string());
                if (!anchor) {
                    return fail(label, "anchor: top_left, top, top_right, left, center, right, bottom_left, bottom or bottom_right");
                }
                out.anchor = anchor;
            } else if (key == "offset") {
                out.offset = {static_cast<f32>(value.number_at("x")), static_cast<f32>(value.number_at("y"))};
            } else if (key == "visible") {
                out.visible = value.as_bool(true);
            } else if (key == "row") {
                out.row = value.as_bool();
            } else if (key == "padding") {
                if (value.is_number()) {
                    const f32 all = static_cast<f32>(value.as_number());
                    out.padding = {all, all, all, all};
                } else if (value.is_array() && (value.items().size() == 2 || value.items().size() == 4)) {
                    const auto& v = value.items();
                    const auto at = [&](std::size_t i) { return static_cast<f32>(v[i].as_number()); };
                    out.padding = v.size() == 2 ? UiPadding{at(0), at(1), at(0), at(1)} : UiPadding{at(0), at(1), at(2), at(3)};
                } else {
                    return fail(label, "padding: a number, [x, y] or [left, top, right, bottom]");
                }
            } else if (key == "gap") {
                out.gap = static_cast<f32>(value.as_number());
            } else if (key == "justify" || key == "items" || key == "align") {
                const std::optional<UiAlign> align = align_named(value.as_string());
                if (!align) {
                    return fail(label, key + ": start, center, end or stretch");
                }
                (key == "justify" ? out.justify : key == "items" ? out.items : out.align) = *align;
            } else if (key == "surface") {
                out.surface = value.as_string();
            } else if (key == "text") {
                out.text = value.as_string();
            } else if (key == "key") {
                out.key = value.as_string();
            } else if (key == "style") {
                out.style = value.as_string();
                const std::vector<std::string_view> styles = screen_text_styles();
                if (!out.style.empty() && std::ranges::find(styles, std::string_view(out.style)) == styles.end()) {
                    return fail(label, "style: body, title, small, muted, emphasis or danger");
                }
            } else if (key == "image") {
                out.image = value.as_string();
            } else if (key == "tint") {
                const std::optional<Color> tint = color(value);
                if (!tint) {
                    return fail(label, "tint: {\"r\": .., \"g\": .., \"b\": .., \"a\": ..}");
                }
                out.tint = *tint;
            } else if (key == "value") {
                out.value = static_cast<f32>(value.as_number());
            } else if (key == "min") {
                out.min = static_cast<f32>(value.as_number());
            } else if (key == "max") {
                out.max = static_cast<f32>(value.as_number(1.0));
            } else if (key == "step") {
                out.step = static_cast<f32>(value.as_number());
            } else if (key == "checked") {
                out.checked = value.as_bool();
            } else if (key == "hint") {
                out.hint = value.as_string();
            } else {
                return fail(label, "no field '" + key + "'");
            }
        }
        if (const JsonValue* children = json.find("children")) {
            if (!screen_kind_holds(out.kind) && !children->items().empty()) {
                return fail(label, "a " + std::string(screen_kind_name(out.kind)) + " holds no elements");
            }
            std::size_t index = 0;
            for (const JsonValue& child : children->items()) {
                ScreenElement made;
                if (!element(child, made, (where.empty() ? "" : where + "/") + std::to_string(index++))) {
                    return false;
                }
                out.children.push_back(std::move(made));
            }
        }
        return true;
    }
};

// --- writing -----------------------------------------------------------------------------------

void write_element(JsonWriter& out, const ScreenElement& e) {
    const ScreenElement defaults{};
    out.begin_object();
    if (!e.id.empty()) out.field("id", e.id);
    out.field("kind", screen_kind_name(e.kind));
    const auto size = [&](std::string_view name, SizeAxis value) {
        if (value.mode == SizeMode::Fixed) {
            out.key(name).value(static_cast<f64>(value.value));
        } else if (!(value.mode == SizeMode::Fit && value.min == 0.0f && value.max == 0.0f)) {
            out.field(name, screen_size_text(value));
        }
    };
    size("width", e.width);
    size("height", e.height);
    if (e.anchor) out.field("anchor", screen_anchor_name(*e.anchor));
    if (e.offset.x != 0.0f || e.offset.y != 0.0f) {
        out.key("offset").begin_object().field("x", static_cast<f64>(e.offset.x)).field("y", static_cast<f64>(e.offset.y)).end_object();
    }
    if (!e.visible) out.field("visible", false);
    if (e.row) out.field("row", true);
    const UiPadding& p = e.padding;
    if (p.left != 0.0f || p.top != 0.0f || p.right != 0.0f || p.bottom != 0.0f) {
        if (p.left == p.top && p.top == p.right && p.right == p.bottom) {
            out.key("padding").value(static_cast<f64>(p.left));
        } else {
            out.key("padding").begin_array();
            for (const f32 v : {p.left, p.top, p.right, p.bottom}) out.value(static_cast<f64>(v));
            out.end_array();
        }
    }
    if (e.gap != 0.0f) out.key("gap").value(static_cast<f64>(e.gap));
    if (e.justify != defaults.justify) out.field("justify", align_name(e.justify));
    if (e.items != defaults.items) out.field("items", align_name(e.items));
    if (!e.surface.empty()) out.field("surface", e.surface);
    if (!e.text.empty()) out.field("text", e.text);
    if (!e.key.empty()) out.field("key", e.key);
    if (!e.style.empty()) out.field("style", e.style);
    if (e.align != defaults.align) out.field("align", align_name(e.align));
    if (!e.image.empty()) out.field("image", e.image);
    if (e.tint != defaults.tint) {
        out.key("tint").begin_object().field("r", static_cast<i32>(e.tint.r)).field("g", static_cast<i32>(e.tint.g))
            .field("b", static_cast<i32>(e.tint.b)).field("a", static_cast<i32>(e.tint.a)).end_object();
    }
    if (e.value != 0.0f) out.key("value").value(static_cast<f64>(e.value));
    if (e.min != 0.0f) out.key("min").value(static_cast<f64>(e.min));
    if (e.max != 1.0f) out.key("max").value(static_cast<f64>(e.max));
    if (e.step != 0.0f) out.key("step").value(static_cast<f64>(e.step));
    if (e.checked) out.field("checked", true);
    if (!e.hint.empty()) out.field("hint", e.hint);
    if (!e.children.empty()) {
        out.key("children").begin_array();
        for (const ScreenElement& child : e.children) {
            write_element(out, child);
        }
        out.end_array();
    }
    out.end_object();
}

template <typename Element>
Element* find_in(Element& element, std::string_view id) {
    if (element.id == id) {
        return &element;
    }
    for (auto& child : element.children) {
        if (Element* found = find_in(child, id)) {
            return found;
        }
    }
    return nullptr;
}

} // namespace

std::string_view screen_kind_name(ScreenKind kind) {
    return kind_names[static_cast<std::size_t>(kind)];
}

std::optional<ScreenKind> screen_kind_named(std::string_view name) {
    for (std::size_t i = 0; i < kind_names.size(); ++i) {
        if (kind_names[i] == name) {
            return static_cast<ScreenKind>(i);
        }
    }
    return std::nullopt;
}

bool screen_kind_holds(ScreenKind kind) {
    return kind == ScreenKind::Box || kind == ScreenKind::Panel;
}

std::vector<std::string_view> screen_text_styles() {
    return {"body", "title", "small", "muted", "emphasis", "danger"};
}

std::vector<std::string_view> screen_surfaces() {
    return {"panel", "card", "header", "subtle", "overlay", "popup", "tooltip", "menu", "row", "selected"};
}

std::string_view screen_anchor_name(UiAnchor anchor) {
    for (const NamedAnchor& named : anchors) {
        if (named.anchor == anchor) {
            return named.name;
        }
    }
    return "top_left";
}

std::optional<UiAnchor> screen_anchor_named(std::string_view name) {
    for (const NamedAnchor& named : anchors) {
        if (named.name == name) {
            return named.anchor;
        }
    }
    return std::nullopt;
}

std::string screen_size_text(SizeAxis size) {
    switch (size.mode) {
    case SizeMode::Fit: return "fit";
    case SizeMode::Grow: return size.value == 1.0f ? std::string("grow") : "grow " + number_text(size.value);
    case SizeMode::Fixed: return number_text(size.value);
    case SizeMode::Percent: return number_text(size.value * 100.0f) + "%";
    }
    return "fit";
}

std::optional<SizeAxis> screen_size_from_text(std::string_view text) {
    while (!text.empty() && text.front() == ' ') text.remove_prefix(1);
    while (!text.empty() && text.back() == ' ') text.remove_suffix(1);
    if (text == "fit" || text.empty()) {
        return fit();
    }
    if (text == "grow") {
        return grow();
    }
    if (text.starts_with("grow ")) {
        if (const std::optional<f32> weight = number(text.substr(5)); weight && *weight > 0.0f) {
            return grow(*weight);
        }
        return std::nullopt;
    }
    if (text.ends_with('%')) {
        if (const std::optional<f32> share = number(text.substr(0, text.size() - 1)); share && *share >= 0.0f) {
            return percent(*share / 100.0f);
        }
        return std::nullopt;
    }
    if (const std::optional<f32> points = number(text); points && *points >= 0.0f) {
        return fixed(*points);
    }
    return std::nullopt;
}

const ScreenElement* ScreenDocument::find(std::string_view id) const {
    return id.empty() ? nullptr : find_in(root, id);
}

ScreenElement* ScreenDocument::find(std::string_view id) {
    return id.empty() ? nullptr : find_in(root, id);
}

bool parse_screen(std::string_view text, ScreenDocument& out, std::string& error) {
    const JsonParseResult parsed = parse_json(text);
    if (!parsed.ok()) {
        error = parsed.error;
        return false;
    }
    const JsonValue& json = *parsed.value;
    if (!json.is_object()) {
        error = "a screen is a JSON object";
        return false;
    }
    if (const std::string schema = json.string_at("schema", "kin.ui/1"); schema != "kin.ui/1") {
        error = "schema '" + schema + "' (kin reads kin.ui/1)";
        return false;
    }
    ScreenDocument made;
    made.theme = json.string_at("theme");
    if (const JsonValue* size = json.find("size")) {
        made.size = {static_cast<f32>(size->number_at("x", 1280.0)), static_cast<f32>(size->number_at("y", 720.0))};
    }
    Reader reader;
    const JsonValue* root = json.find("root");
    if (!root) {
        error = "no root element";
        return false;
    }
    if (!reader.element(*root, made.root, "")) {
        error = reader.error;
        return false;
    }
    out = std::move(made);
    return true;
}

std::string serialize_screen(const ScreenDocument& screen) {
    std::ostringstream text;
    JsonWriter out{text, true};
    out.begin_object();
    out.field("schema", "kin.ui/1");
    if (!screen.theme.empty()) out.field("theme", screen.theme);
    out.key("size").begin_object().field("x", static_cast<f64>(screen.size.x)).field("y", static_cast<f64>(screen.size.y)).end_object();
    out.key("root");
    write_element(out, screen.root);
    out.end_object();
    std::string result = text.str();
    if (result.empty() || result.back() != '\n') {
        result += '\n';
    }
    return result;
}

// --- UiScreen --------------------------------------------------------------------------------

UiScreen::UiScreen(ScreenDocument document) : _document(std::move(document)) {}

void UiScreen::set_document(ScreenDocument document) {
    _document = std::move(document);
}

UiScreen::State& UiScreen::state(std::string_view id) {
    const auto found = _states.find(id);
    if (found != _states.end()) {
        return found->second;
    }
    return _states.emplace(std::string(id), State{}).first->second;
}

const UiScreen::State* UiScreen::find_state(std::string_view id) const {
    const auto found = _states.find(id);
    return found == _states.end() ? nullptr : &found->second;
}

void UiScreen::set_text(std::string_view id, std::string text) {
    State& s = state(id);
    s.text = text;
    s.input.text = std::move(text);
    s.input.caret = s.input.text.size();
    s.input_made = true;
}

void UiScreen::set_value(std::string_view id, f32 value) { state(id).value = value; }
void UiScreen::set_checked(std::string_view id, bool checked) { state(id).checked = checked; }
void UiScreen::set_visible(std::string_view id, bool visible) { state(id).visible = visible; }
void UiScreen::set_image(std::string_view id, std::string path) { state(id).image = std::move(path); }
void UiScreen::set_enabled(std::string_view id, bool enabled) { state(id).enabled = enabled; }

bool UiScreen::clicked(std::string_view id) const {
    const State* s = find_state(id);
    return s && s->clicked;
}

bool UiScreen::changed(std::string_view id) const {
    const State* s = find_state(id);
    return s && s->changed;
}

std::vector<std::string> UiScreen::clicks() const {
    std::vector<std::string> out;
    for (const auto& [id, s] : _states) {
        if (s.clicked) {
            out.push_back(id);
        }
    }
    return out;
}

void UiScreen::clear_events() {
    for (auto& [id, s] : _states) {
        s.clicked = false;
        s.changed = false;
    }
}

f32 UiScreen::value(std::string_view id) const {
    if (const State* s = find_state(id); s && s->value) {
        return *s->value;
    }
    const ScreenElement* e = _document.find(id);
    return e ? e->value : 0.0f;
}

bool UiScreen::checked(std::string_view id) const {
    if (const State* s = find_state(id); s && s->checked) {
        return *s->checked;
    }
    const ScreenElement* e = _document.find(id);
    return e && e->checked;
}

std::string UiScreen::text(std::string_view id) const {
    if (const State* s = find_state(id); s && s->text) {
        return *s->text;
    }
    const ScreenElement* e = _document.find(id);
    return e ? e->text : std::string();
}

bool UiScreen::visible(std::string_view id) const {
    if (const State* s = find_state(id); s && s->visible) {
        return *s->visible;
    }
    const ScreenElement* e = _document.find(id);
    return e && e->visible;
}

void UiScreen::draw(Context& ctx, Rectf bounds, const ScreenAssets& assets) {
    _live.clear();
    _placing.clear();
    ctx.begin_layout(bounds);
    element(ctx, _document.root, "", assets);
    ctx.end_layout();
    // The widgets' results, written back by end_layout().
    for (Live& live : _live) {
        if (live.id.empty()) {
            continue;
        }
        State& s = state(live.id);
        switch (live.kind) {
        case ScreenKind::Button:
            s.clicked = s.clicked || live.button->clicked;
            break;
        case ScreenKind::Toggle:
            if (live.toggle->changed) {
                s.checked = live.toggle->value;
                s.changed = true;
            }
            break;
        case ScreenKind::Slider:
            if (live.slider->changed) {
                s.value = live.slider->value;
                s.changed = true;
            }
            break;
        case ScreenKind::TextInput:
            s.input = live.input->state;
            s.input_made = true;
            if (live.input->result.changed) {
                s.text = live.input->state.text;
                s.changed = true;
            }
            break;
        default:
            break;
        }
    }
    _placed = std::move(_placing);
}

void UiScreen::element(Context& ctx, const ScreenElement& e, const std::string& path, const ScreenAssets& assets) {
    const State* s = e.id.empty() ? nullptr : find_state(e.id);
    if (!(s && s->visible ? *s->visible : e.visible)) {
        return;
    }
    const Id id = make_id(Id{_seed}, std::string_view(path.empty() ? std::string_view("root") : std::string_view(path)));
    BoxStyle box;
    box.layout.width = e.width;
    box.layout.height = e.height;
    if (e.anchor) {
        box.layout.overlay = true;
        box.layout.anchor = *e.anchor;
        box.layout.anchor_offset = e.offset;
    }
    box.interactive = false;
    const bool holds = screen_kind_holds(e.kind);
    if (holds) {
        box.layout.axis = e.row ? UiLayoutAxis::Horizontal : UiLayoutAxis::Vertical;
        box.layout.padding = e.padding;
        box.layout.spacing = e.gap;
        box.layout.main = e.justify;
        box.layout.cross = e.items;
        const std::string_view surface = !e.surface.empty() ? std::string_view(e.surface) : e.kind == ScreenKind::Panel ? "panel" : "";
        if (const SurfaceStyle* look = surface.empty() ? nullptr : theme_surface(ctx.theme(), surface)) {
            box.surface = {.normal = *look, .hovered = *look, .pressed = *look, .focused = *look, .disabled = *look, .selected = *look};
            box.draw_surface = true;
        }
    } else {
        // A leaf: in a box of its size (its bounds are the box's), filling it.
        box.layout.axis = UiLayoutAxis::Horizontal;
        box.layout.cross = UiAlign::Stretch;
    }
    const Context::BoxScope scope = ctx.box(id, box);
    _placing.push_back({.path = path, .bounds = scope.bounds, .hovered = scope.hovered});
    if (holds) {
        std::size_t index = 0;
        for (const ScreenElement& child : e.children) {
            element(ctx, child, path.empty() ? std::to_string(index) : path + "/" + std::to_string(index), assets);
            ++index;
        }
        return;
    }

    // The leaf fills its box where the element has a size, and is as big as it needs where it fits.
    const LayoutStyle inside{.width = e.width.mode == SizeMode::Fit ? fit() : grow(),
                             .height = e.height.mode == SizeMode::Fit ? fit() : grow()};
    const auto shown_text = [&]() -> std::string {
        if (s && s->text) {
            return *s->text;
        }
        if (!e.key.empty()) {
            return assets.translate ? assets.translate(e.key) : tr(e.key);
        }
        return e.text;
    };
    const bool enabled = !(s && s->enabled) || *s->enabled;
    TextStyle text_style = theme_text(ctx.theme(), e.style);
    switch (e.kind) {
    case ScreenKind::Box:
    case ScreenKind::Panel:
        break;
    case ScreenKind::Label: {
        Label label{.id = make_id(id, "label"), .text = shown_text(), .text_style = text_style, .horizontal = e.align};
        ctx.add(std::move(label), inside);
        break;
    }
    case ScreenKind::Button: {
        Live& live = _live.emplace_back(Live{.id = e.id, .kind = e.kind, .button = std::make_unique<Button>(), .toggle = {}, .slider = {}, .input = {}});
        live.button->id = make_id(id, "button");
        live.button->label = shown_text();
        live.button->enabled = enabled;
        if (!e.style.empty()) {
            live.button->text_style = text_style;
        }
        ctx.widget(*live.button, inside);
        break;
    }
    case ScreenKind::Image: {
        const std::string& path_of = s && s->image ? *s->image : e.image;
        const Texture texture = assets.texture && !path_of.empty() ? assets.texture(path_of) : Texture{};
        if (texture) {
            const Vec2i size = texture.size();
            Image image{.texture = texture,
                        .sprite = {.texture = texture, .source = {0.0f, 0.0f, static_cast<f32>(size.x), static_cast<f32>(size.y)}},
                        .size = {static_cast<f32>(size.x), static_cast<f32>(size.y)},
                        .tint = e.tint,
                        .fit = ImageFit::Contain};
            ctx.add(std::move(image), inside);
        } else {
            ctx.spacer(inside);
        }
        break;
    }
    case ScreenKind::Progress: {
        ProgressBar bar;
        bar.value = std::clamp(s && s->value ? *s->value : e.value, 0.0f, 1.0f);
        ctx.add(bar, inside);
        break;
    }
    case ScreenKind::Toggle: {
        Live& live = _live.emplace_back(Live{.id = e.id, .kind = e.kind, .button = {}, .toggle = std::make_unique<Toggle>(), .slider = {}, .input = {}});
        live.toggle->id = make_id(id, "toggle");
        live.toggle->label = shown_text();
        live.toggle->value = s && s->checked ? *s->checked : e.checked;
        live.toggle->enabled = enabled;
        ctx.widget(*live.toggle, inside);
        break;
    }
    case ScreenKind::Slider: {
        Live& live = _live.emplace_back(Live{.id = e.id, .kind = e.kind, .button = {}, .toggle = {}, .slider = std::make_unique<Slider>(), .input = {}});
        live.slider->id = make_id(id, "slider");
        live.slider->min = e.min;
        live.slider->max = e.max;
        live.slider->step = e.step;
        live.slider->value = std::clamp(s && s->value ? *s->value : e.value, std::min(e.min, e.max), std::max(e.min, e.max));
        live.slider->enabled = enabled;
        ctx.widget(*live.slider, inside);
        break;
    }
    case ScreenKind::TextInput: {
        Live& live = _live.emplace_back(Live{.id = e.id, .kind = e.kind, .button = {}, .toggle = {}, .slider = {}, .input = std::make_unique<TextInput>()});
        live.input->id = make_id(id, "input");
        if (s && s->input_made) {
            live.input->state = s->input;
        } else {
            live.input->state.text = shown_text();
            live.input->state.caret = live.input->state.text.size();
        }
        live.input->enabled = enabled;
        const bool empty = live.input->state.text.empty() && !live.input->state.active;
        ctx.widget(*live.input, inside);
        if (empty && !e.hint.empty()) {
            // Its hint, over it while it is empty.
            TextStyle faint = ctx.theme().muted_text;
            const f32 left = ctx.theme().preset.padding_x;
            ctx.add(Label{.id = make_id(id, "hint"), .text = e.hint, .text_style = faint},
                    LayoutStyle{.width = fit(), .height = grow(), .overlay = true, .anchor = UiAnchor::CenterLeft, .anchor_offset = {left, 0.0f}});
        }
        break;
    }
    case ScreenKind::Spacer:
        ctx.spacer(inside);
        break;
    }
}

} // namespace kin::ui2
