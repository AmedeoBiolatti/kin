#include <kin/renderer/svg.hpp>

#include <kin/assets/content.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <fstream>
#include <numbers>
#include <set>
#include <unordered_map>
#include <utility>

namespace kin {

namespace {

// ---- XML: elements and attributes; text, comments and the rest skipped. ----

struct XmlNode {
    std::string name;
    std::vector<std::pair<std::string, std::string>> attributes;
    std::vector<XmlNode> children;

    const std::string* attribute(std::string_view key) const {
        for (const auto& [k, v] : attributes) {
            if (k == key) {
                return &v;
            }
        }
        return nullptr;
    }
};

class XmlParser {
public:
    explicit XmlParser(std::string_view text) : _s(text) {}

    bool parse(XmlNode& root, std::string& error) {
        while (true) {
            skip_space();
            if (starts("<?")) {
                if (!skip_past("?>")) return fail(error, "unclosed <?");
            } else if (starts("<!--")) {
                if (!skip_past("-->")) return fail(error, "unclosed comment");
            } else if (starts("<!")) {
                if (!skip_declaration()) return fail(error, "unclosed <!DOCTYPE");
            } else {
                break;
            }
        }
        if (!starts("<")) {
            return fail(error, "no root element");
        }
        return element(root, 0, error);
    }

private:
    bool fail(std::string& error, const std::string& why) {
        if (error.empty()) {
            error = why + " (at byte " + std::to_string(_i) + ")";
        }
        return false;
    }
    bool starts(std::string_view prefix) const { return _s.substr(_i).starts_with(prefix); }
    void skip_space() {
        while (_i < _s.size() && std::isspace(static_cast<unsigned char>(_s[_i]))) {
            ++_i;
        }
    }
    bool skip_past(std::string_view end) {
        const std::size_t at = _s.find(end, _i);
        if (at == std::string_view::npos) {
            return false;
        }
        _i = at + end.size();
        return true;
    }
    bool skip_declaration() { // <!DOCTYPE ...> with an optional [internal subset]
        int depth = 0;
        for (; _i < _s.size(); ++_i) {
            if (_s[_i] == '[') ++depth;
            else if (_s[_i] == ']') --depth;
            else if (_s[_i] == '>' && depth <= 0) {
                ++_i;
                return true;
            }
        }
        return false;
    }
    std::string name() {
        const std::size_t start = _i;
        while (_i < _s.size() && (std::isalnum(static_cast<unsigned char>(_s[_i])) || _s[_i] == '_' || _s[_i] == ':' ||
                                  _s[_i] == '-' || _s[_i] == '.')) {
            ++_i;
        }
        return std::string{_s.substr(start, _i - start)};
    }
    static std::string decode(std::string_view raw) {
        std::string out;
        for (std::size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] != '&') {
                out += raw[i];
                continue;
            }
            const std::size_t end = raw.find(';', i);
            if (end == std::string_view::npos) {
                out += raw[i];
                continue;
            }
            const std::string_view entity = raw.substr(i + 1, end - i - 1);
            if (entity == "amp") out += '&';
            else if (entity == "lt") out += '<';
            else if (entity == "gt") out += '>';
            else if (entity == "quot") out += '"';
            else if (entity == "apos") out += '\'';
            else if (entity.starts_with('#')) {
                unsigned code = 0;
                const bool hex = entity.size() > 1 && (entity[1] == 'x' || entity[1] == 'X');
                std::from_chars(entity.data() + (hex ? 2 : 1), entity.data() + entity.size(), code, hex ? 16 : 10);
                if (code < 0x80) {
                    out += static_cast<char>(code); // the rest only ever appear in text, which is skipped
                }
            } else {
                out.append(raw.substr(i, end - i + 1));
            }
            i = end;
        }
        return out;
    }
    bool element(XmlNode& node, int depth, std::string& error) {
        if (depth > 256) {
            return fail(error, "elements nested too deeply");
        }
        ++_i; // '<'
        node.name = name();
        if (node.name.empty()) {
            return fail(error, "an element without a name");
        }
        while (true) {
            skip_space();
            if (starts("/>")) {
                _i += 2;
                return true;
            }
            if (starts(">")) {
                ++_i;
                break;
            }
            std::string key = name();
            if (key.empty()) {
                return fail(error, "a bad attribute in <" + node.name + ">");
            }
            skip_space();
            if (!starts("=")) {
                return fail(error, "attribute " + key + " without a value");
            }
            ++_i;
            skip_space();
            if (_i >= _s.size() || (_s[_i] != '"' && _s[_i] != '\'')) {
                return fail(error, "attribute " + key + " without quotes");
            }
            const char quote = _s[_i++];
            const std::size_t end = _s.find(quote, _i);
            if (end == std::string_view::npos) {
                return fail(error, "unclosed attribute " + key);
            }
            node.attributes.emplace_back(std::move(key), decode(_s.substr(_i, end - _i)));
            _i = end + 1;
        }
        while (true) {
            const std::size_t lt = _s.find('<', _i);
            if (lt == std::string_view::npos) {
                return fail(error, "<" + node.name + "> is not closed");
            }
            _i = lt;
            if (starts("</")) {
                _i += 2;
                if (name() != node.name) {
                    return fail(error, "</...> does not match <" + node.name + ">");
                }
                skip_space();
                if (!starts(">")) {
                    return fail(error, "a bad closing tag");
                }
                ++_i;
                return true;
            }
            if (starts("<!--")) {
                if (!skip_past("-->")) return fail(error, "unclosed comment");
            } else if (starts("<![CDATA[")) {
                if (!skip_past("]]>")) return fail(error, "unclosed CDATA");
            } else if (starts("<?")) {
                if (!skip_past("?>")) return fail(error, "unclosed <?");
            } else {
                node.children.emplace_back();
                if (!element(node.children.back(), depth + 1, error)) {
                    return false;
                }
            }
        }
    }

    std::string_view _s;
    std::size_t _i = 0;
};

// ---- Values ----

constexpr f32 pi = std::numbers::pi_v<f32>;

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

std::string lower(std::string_view s) {
    std::string out{s};
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// Numbers separated by spaces and/or commas.
std::vector<f32> numbers(std::string_view s) {
    std::vector<f32> out;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (std::isspace(static_cast<unsigned char>(s[i])) || s[i] == ',')) ++i;
        if (i >= s.size()) break;
        if (s[i] == '+') ++i;
        f32 value = 0.0f;
        const auto result = std::from_chars(s.data() + i, s.data() + s.size(), value);
        if (result.ec != std::errc{}) break;
        out.push_back(value);
        i = static_cast<std::size_t>(result.ptr - s.data());
    }
    return out;
}

std::optional<Color> named_color(std::string_view name) {
    static const std::array<std::pair<std::string_view, Color>, 19> names{{
        {"black", Color::rgb(0, 0, 0)},        {"white", Color::rgb(255, 255, 255)}, {"red", Color::rgb(255, 0, 0)},
        {"green", Color::rgb(0, 128, 0)},      {"lime", Color::rgb(0, 255, 0)},      {"blue", Color::rgb(0, 0, 255)},
        {"yellow", Color::rgb(255, 255, 0)},   {"cyan", Color::rgb(0, 255, 255)},    {"aqua", Color::rgb(0, 255, 255)},
        {"magenta", Color::rgb(255, 0, 255)},  {"fuchsia", Color::rgb(255, 0, 255)}, {"gray", Color::rgb(128, 128, 128)},
        {"grey", Color::rgb(128, 128, 128)},   {"silver", Color::rgb(192, 192, 192)}, {"maroon", Color::rgb(128, 0, 0)},
        {"navy", Color::rgb(0, 0, 128)},       {"purple", Color::rgb(128, 0, 128)},  {"teal", Color::rgb(0, 128, 128)},
        {"orange", Color::rgb(255, 165, 0)},
    }};
    for (const auto& [n, c] : names) {
        if (n == name) {
            return c;
        }
    }
    if (name == "olive") return Color::rgb(128, 128, 0);
    if (name == "transparent") return Color::rgba(0, 0, 0, 0);
    return std::nullopt;
}

struct Paint {
    enum class Kind { None, Color, Current, Unsupported } kind = Kind::None;
    Color color{};
};

std::optional<Paint> parse_paint(std::string_view raw) {
    const std::string s = lower(trim(raw));
    if (s == "none") return Paint{Paint::Kind::None};
    if (s == "currentcolor") return Paint{Paint::Kind::Current};
    if (s.starts_with("url(")) return Paint{Paint::Kind::Unsupported};
    if (s.starts_with('#')) {
        const std::string_view hex = std::string_view{s}.substr(1);
        const auto digit = [&](std::size_t i) {
            unsigned v = 0;
            std::from_chars(hex.data() + i, hex.data() + i + 1, v, 16);
            return v;
        };
        if (!std::all_of(hex.begin(), hex.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); })) {
            return std::nullopt;
        }
        if (hex.size() == 3 || hex.size() == 4) {
            Color c = Color::rgb(static_cast<u8>(digit(0) * 17), static_cast<u8>(digit(1) * 17), static_cast<u8>(digit(2) * 17));
            if (hex.size() == 4) c.a = static_cast<u8>(digit(3) * 17);
            return Paint{Paint::Kind::Color, c};
        }
        if (hex.size() == 6 || hex.size() == 8) {
            const auto byte = [&](std::size_t i) { return static_cast<u8>(digit(i) * 16 + digit(i + 1)); };
            Color c = Color::rgb(byte(0), byte(2), byte(4));
            if (hex.size() == 8) c.a = byte(6);
            return Paint{Paint::Kind::Color, c};
        }
        return std::nullopt;
    }
    if (s.starts_with("rgb(") || s.starts_with("rgba(")) {
        const std::size_t open = s.find('('), close = s.find(')');
        if (close == std::string::npos) return std::nullopt;
        const std::string_view args = std::string_view{s}.substr(open + 1, close - open - 1);
        // Each component a number or a percentage; alpha 0..1 (or a percentage).
        std::vector<f32> values;
        std::vector<bool> percent;
        std::size_t i = 0;
        while (i < args.size()) {
            while (i < args.size() && (std::isspace(static_cast<unsigned char>(args[i])) || args[i] == ',' || args[i] == '/')) ++i;
            if (i >= args.size()) break;
            f32 v = 0.0f;
            const auto r = std::from_chars(args.data() + i, args.data() + args.size(), v);
            if (r.ec != std::errc{}) return std::nullopt;
            i = static_cast<std::size_t>(r.ptr - args.data());
            const bool pct = i < args.size() && args[i] == '%';
            if (pct) ++i;
            values.push_back(v);
            percent.push_back(pct);
        }
        if (values.size() < 3) return std::nullopt;
        const auto channel = [&](std::size_t k) {
            const f32 v = percent[k] ? values[k] * 2.55f : values[k];
            return static_cast<u8>(std::clamp(v, 0.0f, 255.0f) + 0.5f);
        };
        Color c = Color::rgb(channel(0), channel(1), channel(2));
        if (values.size() > 3) {
            const f32 a = percent[3] ? values[3] / 100.0f : values[3];
            c.a = static_cast<u8>(std::clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
        return Paint{Paint::Kind::Color, c};
    }
    if (const std::optional<Color> c = named_color(s)) {
        return Paint{Paint::Kind::Color, *c};
    }
    return std::nullopt;
}

std::optional<Affine2> parse_transform(std::string_view s) {
    Affine2 result{};
    std::size_t i = 0;
    while (true) {
        while (i < s.size() && (std::isspace(static_cast<unsigned char>(s[i])) || s[i] == ',')) ++i;
        if (i >= s.size()) break;
        const std::size_t name_start = i;
        while (i < s.size() && std::isalpha(static_cast<unsigned char>(s[i]))) ++i;
        const std::string_view fn = s.substr(name_start, i - name_start);
        const std::size_t open = s.find('(', i), close = s.find(')', i);
        if (fn.empty() || open == std::string_view::npos || close == std::string_view::npos || close < open) {
            return std::nullopt;
        }
        const std::vector<f32> v = numbers(s.substr(open + 1, close - open - 1));
        i = close + 1;
        Affine2 m{};
        const auto arg = [&](std::size_t k, f32 fallback) { return k < v.size() ? v[k] : fallback; };
        if (fn == "matrix" && v.size() == 6) {
            m = Affine2{v[0], v[1], v[2], v[3], v[4], v[5]};
        } else if (fn == "translate" && !v.empty()) {
            m = Affine2::translation({v[0], arg(1, 0.0f)});
        } else if (fn == "scale" && !v.empty()) {
            m = Affine2::scaling({v[0], arg(1, v[0])});
        } else if (fn == "rotate" && !v.empty()) {
            const Vec2f c{arg(1, 0.0f), arg(2, 0.0f)};
            m = Affine2::translation(c) * Affine2::rotation(v[0]) * Affine2::translation({-c.x, -c.y});
        } else if (fn == "skewX" && v.size() == 1) {
            m = Affine2{1.0f, 0.0f, std::tan(v[0] * pi / 180.0f), 1.0f, 0.0f, 0.0f};
        } else if (fn == "skewY" && v.size() == 1) {
            m = Affine2{1.0f, std::tan(v[0] * pi / 180.0f), 0.0f, 1.0f, 0.0f, 0.0f};
        } else {
            return std::nullopt;
        }
        result = result * m;
    }
    return result;
}

// ---- Reading ----

struct Style {
    Paint fill{Paint::Kind::Color, Color::rgb(0, 0, 0)}; // SVG fills black by default
    f32 fill_opacity = 1.0f;
    FillRule fill_rule = FillRule::NonZero;
    Paint stroke{};
    f32 stroke_opacity = 1.0f;
    StrokeStyle stroke_style{};
    f32 opacity = 1.0f;
    Color color = Color::rgb(0, 0, 0);
    bool visible = true;
    bool displayed = true;
};

class Reader {
public:
    explicit Reader(std::vector<std::string>* warnings) : _warnings(warnings) {}

    void index(const XmlNode& node) {
        if (const std::string* id = node.attribute("id")) {
            _ids.emplace(*id, &node);
        }
        for (const XmlNode& child : node.children) {
            index(child);
        }
    }

    void walk(const XmlNode& node, Style style, const Affine2& parent, int depth) {
        if (depth > 64) {
            warn("<use> nested too deeply (a reference cycle?)");
            return;
        }
        const std::string name = local_name(node.name);
        if (name == "defs" || name == "symbol" || name == "title" || name == "desc" || name == "metadata" ||
            name == "linearGradient" || name == "radialGradient" || name == "pattern" || name == "clipPath" ||
            name == "mask" || name == "filter" || name == "marker" || name.starts_with("sodipodi") ||
            name.starts_with("inkscape")) {
            return; // not drawn where it stands
        }
        if (name == "text" || name == "image" || name == "style" || name == "foreignObject") {
            warn("<" + name + "> is not read");
            return;
        }
        apply_presentation(node, style);
        if (!style.displayed) {
            return;
        }
        Affine2 transform = parent;
        if (const std::string* t = node.attribute("transform")) {
            if (const std::optional<Affine2> m = parse_transform(*t)) {
                transform = transform * *m;
            } else {
                warn("a transform not understood: " + *t);
            }
        }
        if (node.attribute("clip-path") || node.attribute("mask") || node.attribute("filter")) {
            warn("clip paths, masks and filters are not applied");
        }

        if (name == "g" || name == "svg" || name == "a" || name == "switch") {
            for (const XmlNode& child : node.children) {
                walk(child, style, transform, depth);
            }
            return;
        }
        if (name == "use") {
            const std::string* href = node.attribute("href");
            if (!href) href = node.attribute("xlink:href");
            if (!href || !href->starts_with('#')) {
                warn("<use> without a #reference");
                return;
            }
            const auto target = _ids.find(href->substr(1));
            if (target == _ids.end()) {
                warn("<use> of a missing id: " + *href);
                return;
            }
            const Affine2 at = transform * Affine2::translation({length(node, "x"), length(node, "y")});
            const XmlNode& referenced = *target->second;
            if (local_name(referenced.name) == "symbol") {
                Style inner = style;
                apply_presentation(referenced, inner);
                for (const XmlNode& child : referenced.children) {
                    walk(child, inner, at, depth + 1);
                }
            } else {
                walk(referenced, style, at, depth + 1);
            }
            return;
        }

        Path path;
        bool fillable = true;
        if (name == "path") {
            const std::string* d = node.attribute("d");
            if (!d) return;
            std::string error;
            const std::optional<Path> parsed = Path::parse_svg(*d, &error);
            if (!parsed) {
                warn("path data not understood (" + error + ")");
                return;
            }
            path = *parsed;
        } else if (name == "rect") {
            const f32 x = length(node, "x"), y = length(node, "y");
            const f32 w = length(node, "width"), h = length(node, "height");
            if (w <= 0.0f || h <= 0.0f) return;
            f32 rx = length(node, "rx", -1.0f), ry = length(node, "ry", -1.0f);
            if (rx < 0.0f) rx = ry;
            if (ry < 0.0f) ry = rx;
            rx = std::clamp(rx, 0.0f, w * 0.5f);
            ry = std::clamp(ry, 0.0f, h * 0.5f);
            if (rx <= 0.0f || ry <= 0.0f) {
                path = Path::rect({x, y, w, h});
            } else if (rx == ry) {
                path = Path::rounded_rect({x, y, w, h}, rx);
            } else {
                path.move_to({x + rx, y})
                    .line_to({x + w - rx, y})
                    .arc_to({rx, ry}, 0.0f, false, true, {x + w, y + ry})
                    .line_to({x + w, y + h - ry})
                    .arc_to({rx, ry}, 0.0f, false, true, {x + w - rx, y + h})
                    .line_to({x + rx, y + h})
                    .arc_to({rx, ry}, 0.0f, false, true, {x, y + h - ry})
                    .line_to({x, y + ry})
                    .arc_to({rx, ry}, 0.0f, false, true, {x + rx, y})
                    .close();
            }
        } else if (name == "circle") {
            const f32 r = length(node, "r");
            if (r <= 0.0f) return;
            path = Path::circle({length(node, "cx"), length(node, "cy")}, r);
        } else if (name == "ellipse") {
            const f32 rx = length(node, "rx"), ry = length(node, "ry");
            if (rx <= 0.0f || ry <= 0.0f) return;
            path = Path::ellipse({length(node, "cx"), length(node, "cy")}, {rx, ry});
        } else if (name == "line") {
            path = Path::line({length(node, "x1"), length(node, "y1")}, {length(node, "x2"), length(node, "y2")});
            fillable = false;
        } else if (name == "polyline" || name == "polygon") {
            const std::string* points = node.attribute("points");
            if (!points) return;
            const std::vector<f32> v = numbers(*points);
            std::vector<Vec2f> pts;
            for (std::size_t i = 0; i + 1 < v.size(); i += 2) {
                pts.push_back({v[i], v[i + 1]});
            }
            if (pts.empty()) return;
            path = name == "polygon" ? Path::polygon(pts) : Path::polyline(pts);
        } else {
            if (_seen_unknown.insert(name).second) {
                warn("<" + name + "> is not read");
            }
            return;
        }
        if (!style.visible) {
            return;
        }

        ShapeElement element{.path = std::move(path), .fill_rule = style.fill_rule, .transform = transform};
        if (const std::string* id = node.attribute("id")) {
            element.id = *id;
        }
        if (fillable) {
            element.fill = resolve(style.fill, style, style.fill_opacity);
        }
        element.stroke = resolve(style.stroke, style, style.stroke_opacity);
        element.stroke_style = style.stroke_style;
        if (element.fill || (element.stroke && element.stroke_style.width > 0.0f)) {
            if (!element.stroke || element.stroke_style.width <= 0.0f) {
                element.stroke.reset();
            }
            shape.add(std::move(element));
        }
    }

    Shape shape;

private:
    static std::string local_name(const std::string& name) {
        const std::size_t colon = name.find(':');
        return colon == std::string::npos || name.starts_with("sodipodi") || name.starts_with("inkscape")
                   ? name
                   : name.substr(colon + 1);
    }

    void warn(const std::string& message) {
        if (_warnings && _warned.insert(message).second) {
            _warnings->push_back(message);
        }
    }

    f32 parse_length(std::string_view raw, f32 fallback) {
        raw = trim(raw);
        if (raw.empty()) return fallback;
        if (raw.front() == '+') raw.remove_prefix(1);
        f32 value = 0.0f;
        const auto result = std::from_chars(raw.data(), raw.data() + raw.size(), value);
        if (result.ec != std::errc{}) {
            warn("a length not understood: " + std::string{raw});
            return fallback;
        }
        const std::string_view unit = trim(raw.substr(static_cast<std::size_t>(result.ptr - raw.data())));
        if (!unit.empty() && unit != "px") {
            warn("lengths in " + std::string{unit} + " are read as px");
        }
        return value;
    }

    f32 length(const XmlNode& node, std::string_view key, f32 fallback = 0.0f) {
        const std::string* v = node.attribute(key);
        return v ? parse_length(*v, fallback) : fallback;
    }

    std::optional<Color> resolve(const Paint& paint, const Style& style, f32 opacity) {
        Color c{};
        switch (paint.kind) {
        case Paint::Kind::None: return std::nullopt;
        case Paint::Kind::Unsupported:
            warn("gradient and pattern paints are not read (drawn unpainted)");
            return std::nullopt;
        case Paint::Kind::Current: c = style.color; break;
        case Paint::Kind::Color: c = paint.color; break;
        }
        c.a = static_cast<u8>(std::clamp(static_cast<f32>(c.a) * opacity * style.opacity, 0.0f, 255.0f) + 0.5f);
        return c;
    }

    void declare(Style& style, std::string_view key, std::string_view value) {
        value = trim(value);
        if (value == "inherit") return;
        const auto opacity = [&](f32& out) {
            const std::vector<f32> v = numbers(value);
            if (!v.empty()) out = std::clamp(value.ends_with('%') ? v[0] / 100.0f : v[0], 0.0f, 1.0f);
        };
        const auto paint = [&](Paint& out) {
            if (const std::optional<Paint> p = parse_paint(value)) {
                out = *p;
            } else {
                warn("a colour not understood: " + std::string{value});
            }
        };
        if (key == "fill") paint(style.fill);
        else if (key == "stroke") paint(style.stroke);
        else if (key == "fill-opacity") opacity(style.fill_opacity);
        else if (key == "stroke-opacity") opacity(style.stroke_opacity);
        else if (key == "opacity") {
            f32 o = 1.0f;
            opacity(o);
            style.opacity *= o;
        } else if (key == "fill-rule") style.fill_rule = value == "evenodd" ? FillRule::EvenOdd : FillRule::NonZero;
        else if (key == "stroke-width") style.stroke_style.width = std::max(0.0f, parse_length(value, 1.0f));
        else if (key == "stroke-miterlimit") style.stroke_style.miter_limit = std::max(1.0f, parse_length(value, 4.0f));
        else if (key == "stroke-linejoin") {
            style.stroke_style.join = value == "round" ? LineJoin::Round : value == "bevel" ? LineJoin::Bevel : LineJoin::Miter;
        } else if (key == "stroke-linecap") {
            style.stroke_style.cap = value == "round" ? LineCap::Round : value == "square" ? LineCap::Square : LineCap::Butt;
        } else if (key == "color") {
            if (const std::optional<Paint> p = parse_paint(value); p && p->kind == Paint::Kind::Color) style.color = p->color;
        } else if (key == "display") style.displayed = value != "none";
        else if (key == "visibility") style.visible = value == "visible";
        else if (key == "stroke-dasharray" && value != "none") warn("dashed strokes are drawn solid");
        else if ((key == "clip-path" || key == "mask" || key == "filter") && value != "none") {
            warn("clip paths, masks and filters are not applied");
        }
    }

    // Attributes, then style="" (which wins).
    void apply_presentation(const XmlNode& node, Style& style) {
        style.displayed = true; // display does not inherit
        for (const auto& [key, value] : node.attributes) {
            declare(style, key, value);
        }
        if (const std::string* css = node.attribute("style")) {
            std::string_view rest{*css};
            while (!rest.empty()) {
                const std::size_t semi = rest.find(';');
                const std::string_view item = rest.substr(0, semi);
                const std::size_t colon = item.find(':');
                if (colon != std::string_view::npos) {
                    declare(style, trim(item.substr(0, colon)), item.substr(colon + 1));
                }
                if (semi == std::string_view::npos) break;
                rest.remove_prefix(semi + 1);
            }
        }
    }

    std::vector<std::string>* _warnings = nullptr;
    std::set<std::string> _warned;
    std::set<std::string> _seen_unknown;
    std::unordered_map<std::string, const XmlNode*> _ids;
};

// ---- Writing ----

void number(std::string& out, f32 value) {
    if (value == 0.0f) value = 0.0f;
    std::array<char, 32> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    out.append(buffer.data(), result.ptr);
}

std::string escape(std::string_view s) {
    std::string out;
    for (const char c : s) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out += c;
        }
    }
    return out;
}

void color_attributes(std::string& out, std::string_view paint, Color c) {
    static constexpr char hex[] = "0123456789abcdef";
    out += ' ';
    out += paint;
    out += "=\"#";
    for (const u8 v : {c.r, c.g, c.b}) {
        out += hex[v >> 4];
        out += hex[v & 15];
    }
    out += '"';
    if (c.a != 255) {
        out += ' ';
        out += paint;
        out += "-opacity=\"";
        number(out, static_cast<f32>(c.a) / 255.0f);
        out += '"';
    }
}

} // namespace

std::optional<Shape> read_svg(std::string_view svg, std::string* error, std::vector<std::string>* warnings) {
    XmlNode root;
    std::string why;
    if (!XmlParser{svg}.parse(root, why)) {
        if (error) *error = why;
        return std::nullopt;
    }
    if (root.name != "svg" && !root.name.ends_with(":svg")) {
        if (error) *error = "the root element is <" + root.name + ">, not <svg>";
        return std::nullopt;
    }
    Reader reader{warnings};
    reader.index(root);
    reader.walk(root, Style{}, Affine2{}, 0);
    Shape shape = std::move(reader.shape);
    if (const std::string* box = root.attribute("viewBox")) {
        const std::vector<f32> v = numbers(*box);
        if (v.size() == 4) shape.view_box = {v[0], v[1], v[2], v[3]};
    }
    if (shape.view_box.w <= 0.0f) {
        const std::string* w = root.attribute("width");
        const std::string* h = root.attribute("height");
        if (w && h) {
            const std::vector<f32> vw = numbers(*w), vh = numbers(*h);
            if (!vw.empty() && !vh.empty()) shape.view_box = {0.0f, 0.0f, vw[0], vh[0]};
        }
    }
    return shape;
}

std::optional<Shape> load_svg(const std::filesystem::path& path, std::string* error, std::vector<std::string>* warnings) {
    const std::optional<std::string> text = read_content_file(path);
    if (!text) {
        if (error) *error = "cannot open " + path.string();
        return std::nullopt;
    }
    return read_svg(*text, error, warnings);
}

std::string write_svg(const Shape& shape) {
    const Rectf box = shape.view_box.w > 0.0f && shape.view_box.h > 0.0f ? shape.view_box : shape.bounds();
    std::string out = "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"";
    number(out, box.x);
    out += ' ';
    number(out, box.y);
    out += ' ';
    number(out, box.w);
    out += ' ';
    number(out, box.h);
    out += "\" width=\"";
    number(out, box.w);
    out += "\" height=\"";
    number(out, box.h);
    out += "\">\n";
    for (const ShapeElement& element : shape.elements) {
        // A circle, ellipse or rectangle as itself (it reads back as one, to be
        // drawn whole); anything else as a path.
        const std::optional<PathPrimitive>& shape = element.path.primitive();
        const bool simple = shape && shape->transform.is_translation();
        const auto attribute = [&](std::string_view key, f32 value) {
            out += ' ';
            out += key;
            out += "=\"";
            number(out, value);
            out += '"';
        };
        if (simple && shape->kind == PathPrimitive::Kind::Ellipse) {
            out += "  <ellipse";
        } else if (simple && shape->half_size.x == shape->half_size.y && shape->radius == shape->half_size.x) {
            out += "  <circle";
        } else if (simple) {
            out += "  <rect";
        } else {
            out += "  <path";
        }
        if (!element.id.empty()) {
            out += " id=\"" + escape(element.id) + "\"";
        }
        if (!simple) {
            out += " d=\"" + element.path.to_svg() + "\"";
        } else if (shape->kind == PathPrimitive::Kind::Ellipse) {
            attribute("cx", shape->transform.tx);
            attribute("cy", shape->transform.ty);
            attribute("rx", shape->half_size.x);
            attribute("ry", shape->half_size.y);
        } else if (shape->half_size.x == shape->half_size.y && shape->radius == shape->half_size.x) {
            attribute("cx", shape->transform.tx);
            attribute("cy", shape->transform.ty);
            attribute("r", shape->radius);
        } else {
            attribute("x", shape->transform.tx - shape->half_size.x);
            attribute("y", shape->transform.ty - shape->half_size.y);
            attribute("width", 2.0f * shape->half_size.x);
            attribute("height", 2.0f * shape->half_size.y);
            if (shape->radius > 0.0f) {
                attribute("rx", shape->radius);
            }
        }
        if (element.fill) {
            color_attributes(out, "fill", *element.fill);
            if (element.fill_rule == FillRule::EvenOdd) out += " fill-rule=\"evenodd\"";
        } else {
            out += " fill=\"none\"";
        }
        if (element.stroke) {
            const StrokeStyle& s = element.stroke_style;
            color_attributes(out, "stroke", *element.stroke);
            if (s.width != 1.0f) {
                out += " stroke-width=\"";
                number(out, s.width);
                out += '"';
            }
            if (s.join != LineJoin::Miter) out += s.join == LineJoin::Round ? " stroke-linejoin=\"round\"" : " stroke-linejoin=\"bevel\"";
            if (s.cap != LineCap::Butt) out += s.cap == LineCap::Round ? " stroke-linecap=\"round\"" : " stroke-linecap=\"square\"";
            if (s.miter_limit != 4.0f) {
                out += " stroke-miterlimit=\"";
                number(out, s.miter_limit);
                out += '"';
            }
        }
        if (!element.transform.is_identity()) {
            const Affine2& m = element.transform;
            out += " transform=\"matrix(";
            const std::array<f32, 6> v{m.a, m.b, m.c, m.d, m.tx, m.ty};
            for (std::size_t i = 0; i < v.size(); ++i) {
                if (i) out += ' ';
                number(out, v[i]);
            }
            out += ")\"";
        }
        out += "/>\n";
    }
    out += "</svg>\n";
    return out;
}

bool save_svg(const Shape& shape, const std::filesystem::path& path) {
    std::ofstream out{path, std::ios::binary};
    if (!out) {
        return false;
    }
    out << write_svg(shape);
    return static_cast<bool>(out);
}

} // namespace kin
