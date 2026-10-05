#include <kin/renderer/path.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <numbers>

namespace kin {

namespace {

constexpr f32 pi = std::numbers::pi_v<f32>;
// A cubic's control points for a quarter circle sit this far along the tangents.
constexpr f32 kappa = 0.5522847498f;

Vec2f add(Vec2f a, Vec2f b) { return {a.x + b.x, a.y + b.y}; }
Vec2f sub(Vec2f a, Vec2f b) { return {a.x - b.x, a.y - b.y}; }
Vec2f scale(Vec2f a, f32 s) { return {a.x * s, a.y * s}; }
// Curves are split into equal steps of their parameter, as many as Wang's
// formula says keep every point within `tolerance` of the curve: close to the
// fewest that do (halving until flat overshoots to a power of two).
f32 length(Vec2f a) { return std::sqrt(a.x * a.x + a.y * a.y); }

int curve_steps(f32 second_difference, f32 degree_factor, f32 tolerance) {
    const f32 n = std::ceil(std::sqrt(degree_factor * second_difference / tolerance));
    return std::clamp(static_cast<int>(n), 1, 1024);
}

void flatten_quad(std::vector<Vec2f>& out, Vec2f p0, Vec2f p1, Vec2f p2, f32 tolerance) {
    const int n = curve_steps(length(add(sub(p0, scale(p1, 2.0f)), p2)), 0.25f, tolerance);
    for (int i = 1; i <= n; ++i) {
        const f32 t = static_cast<f32>(i) / static_cast<f32>(n), u = 1.0f - t;
        out.push_back(i == n ? p2 : add(add(scale(p0, u * u), scale(p1, 2.0f * u * t)), scale(p2, t * t)));
    }
}

void flatten_cubic(std::vector<Vec2f>& out, Vec2f p0, Vec2f p1, Vec2f p2, Vec2f p3, f32 tolerance) {
    const f32 m = std::max(length(add(sub(p0, scale(p1, 2.0f)), p2)), length(add(sub(p1, scale(p2, 2.0f)), p3)));
    const int n = curve_steps(m, 0.75f, tolerance);
    for (int i = 1; i <= n; ++i) {
        const f32 t = static_cast<f32>(i) / static_cast<f32>(n), u = 1.0f - t;
        out.push_back(i == n ? p3
                             : add(add(scale(p0, u * u * u), scale(p1, 3.0f * u * u * t)),
                                   add(scale(p2, 3.0f * u * t * t), scale(p3, t * t * t))));
    }
}

f32 vector_angle(Vec2f u, Vec2f v) {
    return std::atan2(u.x * v.y - u.y * v.x, u.x * v.x + u.y * v.y);
}

} // namespace

void Path::ensure_start() {
    if (_verbs.empty()) {
        move_to({});
    } else if (_verbs.back() == Verb::Close) {
        move_to(_subpath_start);
    }
}

Path& Path::move_to(Vec2f p) {
    _verbs.push_back(Verb::Move);
    _points.push_back(p);
    _subpath_start = p;
    return *this;
}

Path& Path::line_to(Vec2f p) {
    ensure_start();
    _verbs.push_back(Verb::Line);
    _points.push_back(p);
    return *this;
}

Path& Path::quad_to(Vec2f control, Vec2f p) {
    ensure_start();
    _verbs.push_back(Verb::Quad);
    _points.push_back(control);
    _points.push_back(p);
    return *this;
}

Path& Path::cubic_to(Vec2f control1, Vec2f control2, Vec2f p) {
    ensure_start();
    _verbs.push_back(Verb::Cubic);
    _points.push_back(control1);
    _points.push_back(control2);
    _points.push_back(p);
    return *this;
}

Path& Path::arc_to(Vec2f radii, f32 rotation, bool large_arc, bool sweep, Vec2f p) {
    // SVG 1.1 implementation notes F.6.5: from the end points to the centre.
    ensure_start();
    const Vec2f from = _points.back();
    f32 rx = std::abs(radii.x), ry = std::abs(radii.y);
    if (from == p) {
        return *this;
    }
    if (rx < 1e-6f || ry < 1e-6f) {
        return line_to(p);
    }
    const f32 phi = rotation * pi / 180.0f;
    const f32 cos_phi = std::cos(phi), sin_phi = std::sin(phi);
    const f32 dx2 = (from.x - p.x) * 0.5f, dy2 = (from.y - p.y) * 0.5f;
    const f32 x1 = cos_phi * dx2 + sin_phi * dy2;
    const f32 y1 = -sin_phi * dx2 + cos_phi * dy2;
    const f32 lambda = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry);
    if (lambda > 1.0f) { // radii too small to reach: scaled up until they just do
        rx *= std::sqrt(lambda);
        ry *= std::sqrt(lambda);
    }
    const f32 num = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1;
    const f32 den = rx * rx * y1 * y1 + ry * ry * x1 * x1;
    f32 coef = den > 0.0f ? std::sqrt(std::max(0.0f, num / den)) : 0.0f;
    if (large_arc == sweep) {
        coef = -coef;
    }
    const f32 cxp = coef * rx * y1 / ry;
    const f32 cyp = -coef * ry * x1 / rx;
    const Vec2f center{cos_phi * cxp - sin_phi * cyp + (from.x + p.x) * 0.5f,
                       sin_phi * cxp + cos_phi * cyp + (from.y + p.y) * 0.5f};
    const Vec2f u{(x1 - cxp) / rx, (y1 - cyp) / ry};
    const Vec2f v{(-x1 - cxp) / rx, (-y1 - cyp) / ry};
    const f32 theta = vector_angle({1.0f, 0.0f}, u);
    f32 delta = vector_angle(u, v);
    if (!sweep && delta > 0.0f) {
        delta -= 2.0f * pi;
    } else if (sweep && delta < 0.0f) {
        delta += 2.0f * pi;
    }
    // At most a quarter turn per cubic.
    const int segments = std::max(1, static_cast<int>(std::ceil(std::abs(delta) / (pi * 0.5f) - 1e-4f)));
    const f32 step = delta / static_cast<f32>(segments);
    const f32 k = 4.0f / 3.0f * std::tan(step / 4.0f);
    const auto map = [&](f32 ux, f32 uy) {
        return Vec2f{center.x + cos_phi * rx * ux - sin_phi * ry * uy, center.y + sin_phi * rx * ux + cos_phi * ry * uy};
    };
    for (int i = 0; i < segments; ++i) {
        const f32 a0 = theta + step * static_cast<f32>(i), a1 = a0 + step;
        const f32 c0 = std::cos(a0), s0 = std::sin(a0), c1 = std::cos(a1), s1 = std::sin(a1);
        const Vec2f end = i + 1 == segments ? p : map(c1, s1);
        cubic_to(map(c0 - k * s0, s0 + k * c0), map(c1 + k * s1, s1 - k * c1), end);
    }
    return *this;
}

Path& Path::close() {
    if (!_verbs.empty() && _verbs.back() != Verb::Close) {
        _verbs.push_back(Verb::Close);
    }
    return *this;
}

Path& Path::append(const Path& other, const Affine2& transform) {
    std::size_t at = 0;
    for (const Verb verb : other._verbs) {
        const auto next = [&] { return transform.apply(other._points[at++]); };
        switch (verb) {
        case Verb::Move: move_to(next()); break;
        case Verb::Line: line_to(next()); break;
        case Verb::Quad: {
            const Vec2f c = next();
            quad_to(c, next());
            break;
        }
        case Verb::Cubic: {
            const Vec2f c1 = next(), c2 = next();
            cubic_to(c1, c2, next());
            break;
        }
        case Verb::Close: close(); break;
        }
    }
    return *this;
}

Path Path::rect(Rectf r) {
    Path path;
    path.move_to({r.x, r.y}).line_to({r.x + r.w, r.y}).line_to({r.x + r.w, r.y + r.h}).line_to({r.x, r.y + r.h}).close();
    return path;
}

Path Path::rounded_rect(Rectf r, f32 radius) {
    radius = std::clamp(radius, 0.0f, std::min(std::abs(r.w), std::abs(r.h)) * 0.5f);
    if (radius <= 0.0f) {
        return rect(r);
    }
    const f32 k = radius * (1.0f - kappa);
    const f32 x0 = r.x, y0 = r.y, x1 = r.x + r.w, y1 = r.y + r.h;
    Path path;
    path.move_to({x0 + radius, y0})
        .line_to({x1 - radius, y0})
        .cubic_to({x1 - k, y0}, {x1, y0 + k}, {x1, y0 + radius})
        .line_to({x1, y1 - radius})
        .cubic_to({x1, y1 - k}, {x1 - k, y1}, {x1 - radius, y1})
        .line_to({x0 + radius, y1})
        .cubic_to({x0 + k, y1}, {x0, y1 - k}, {x0, y1 - radius})
        .line_to({x0, y0 + radius})
        .cubic_to({x0, y0 + k}, {x0 + k, y0}, {x0 + radius, y0})
        .close();
    return path;
}

Path Path::circle(Vec2f center, f32 radius) {
    return ellipse(center, {radius, radius});
}

Path Path::ellipse(Vec2f c, Vec2f r) {
    const f32 kx = r.x * kappa, ky = r.y * kappa;
    Path path;
    path.move_to({c.x + r.x, c.y})
        .cubic_to({c.x + r.x, c.y + ky}, {c.x + kx, c.y + r.y}, {c.x, c.y + r.y})
        .cubic_to({c.x - kx, c.y + r.y}, {c.x - r.x, c.y + ky}, {c.x - r.x, c.y})
        .cubic_to({c.x - r.x, c.y - ky}, {c.x - kx, c.y - r.y}, {c.x, c.y - r.y})
        .cubic_to({c.x + kx, c.y - r.y}, {c.x + r.x, c.y - ky}, {c.x + r.x, c.y})
        .close();
    return path;
}

Path Path::line(Vec2f a, Vec2f b) {
    Path path;
    path.move_to(a).line_to(b);
    return path;
}

Path Path::polyline(std::span<const Vec2f> points) {
    Path path;
    for (std::size_t i = 0; i < points.size(); ++i) {
        i == 0 ? path.move_to(points[i]) : path.line_to(points[i]);
    }
    return path;
}

Path Path::polygon(std::span<const Vec2f> points) {
    Path path = polyline(points);
    return path.close();
}

Path Path::arc(Vec2f center, f32 radius, f32 start, f32 end) {
    Path path;
    const auto at = [&](f32 degrees) {
        const f32 a = degrees * pi / 180.0f;
        return Vec2f{center.x + radius * std::cos(a), center.y + radius * std::sin(a)};
    };
    path.move_to(at(start));
    f32 sweep = end - start;
    if (std::abs(sweep) >= 360.0f) {
        sweep = sweep > 0.0f ? 360.0f : -360.0f;
    }
    // Two halves, so a whole circle is not a zero-length arc.
    const f32 mid = start + sweep * 0.5f;
    path.arc_to({radius, radius}, 0.0f, false, sweep > 0.0f, at(mid));
    path.arc_to({radius, radius}, 0.0f, false, sweep > 0.0f, at(start + sweep));
    return path;
}

Path Path::pie(Vec2f center, f32 radius, f32 start, f32 end) {
    Path path;
    path.move_to(center);
    path.append(arc(center, radius, start, end));
    // arc() began a new subpath: join it to the centre instead.
    path._verbs[1] = Verb::Line;
    path._subpath_start = center;
    return path.close();
}

Path Path::regular_polygon(Vec2f center, f32 radius, i32 sides, f32 rotation) {
    std::vector<Vec2f> points;
    sides = std::max(3, sides);
    for (i32 i = 0; i < sides; ++i) {
        const f32 a = (rotation - 90.0f + 360.0f * static_cast<f32>(i) / static_cast<f32>(sides)) * pi / 180.0f;
        points.push_back({center.x + radius * std::cos(a), center.y + radius * std::sin(a)});
    }
    return polygon(points);
}

Path Path::star(Vec2f center, f32 outer_radius, f32 inner_radius, i32 points, f32 rotation) {
    std::vector<Vec2f> corners;
    points = std::max(2, points);
    for (i32 i = 0; i < points * 2; ++i) {
        const f32 r = i % 2 == 0 ? outer_radius : inner_radius;
        const f32 a = (rotation - 90.0f + 180.0f * static_cast<f32>(i) / static_cast<f32>(points)) * pi / 180.0f;
        corners.push_back({center.x + r * std::cos(a), center.y + r * std::sin(a)});
    }
    return polygon(corners);
}

Rectf Path::bounds() const {
    if (_points.empty()) {
        return {};
    }
    f32 x0 = _points[0].x, y0 = _points[0].y, x1 = x0, y1 = y0;
    for (const Vec2f& p : _points) {
        x0 = std::min(x0, p.x);
        y0 = std::min(y0, p.y);
        x1 = std::max(x1, p.x);
        y1 = std::max(y1, p.y);
    }
    return {x0, y0, x1 - x0, y1 - y0};
}

Path Path::transformed(const Affine2& transform) const {
    Path out;
    out.append(*this, transform);
    return out;
}

void Path::flatten(std::vector<PathContour>& out, f32 tolerance, const Affine2& transform) const {
    tolerance = std::max(tolerance, 1e-4f);
    PathContour* contour = nullptr;
    std::size_t at = 0;
    const auto next = [&] { return transform.apply(_points[at++]); };
    // A lone point is kept too: a round or square cap draws it as a dot.
    const auto finish = [&] { contour = nullptr; };
    const auto current = [&] { return contour->points.back(); };
    for (const Verb verb : _verbs) {
        switch (verb) {
        case Verb::Move:
            finish();
            out.push_back({});
            contour = &out.back();
            contour->points.push_back(next());
            break;
        case Verb::Line:
            contour->points.push_back(next());
            break;
        case Verb::Quad: {
            const Vec2f c = next(), p = next();
            flatten_quad(contour->points, current(), c, p, tolerance);
            break;
        }
        case Verb::Cubic: {
            const Vec2f c1 = next(), c2 = next(), p = next();
            flatten_cubic(contour->points, current(), c1, c2, p, tolerance);
            break;
        }
        case Verb::Close:
            if (contour) {
                contour->closed = true;
                // The closing point repeats the first: the contour closes by itself.
                if (contour->points.size() > 1 && contour->points.back() == contour->points.front()) {
                    contour->points.pop_back();
                }
            }
            finish();
            break;
        }
    }
}

namespace {

struct SvgPathParser {
    std::string_view s;
    std::size_t i = 0;

    void skip() {
        while (i < s.size() && (s[i] == ' ' || s[i] == ',' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) {
            ++i;
        }
    }
    bool at_number() {
        skip();
        return i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '-' || s[i] == '+' || s[i] == '.');
    }
    bool number(f32& out) {
        skip();
        if (i >= s.size()) {
            return false;
        }
        // from_chars takes no leading '+'.
        std::size_t start = i;
        if (s[i] == '+') {
            start = ++i;
        }
        const auto result = std::from_chars(s.data() + start, s.data() + s.size(), out);
        if (result.ec != std::errc{}) {
            return false;
        }
        i = static_cast<std::size_t>(result.ptr - s.data());
        return true;
    }
    bool flag(bool& out) { // arc flags may run together: "a10 10 0 0110 10"
        skip();
        if (i < s.size() && (s[i] == '0' || s[i] == '1')) {
            out = s[i++] == '1';
            return true;
        }
        return false;
    }
    bool point(Vec2f& out) { return number(out.x) && number(out.y); }
};

void append_number(std::string& out, f32 value) {
    if (value == 0.0f) {
        value = 0.0f; // no "-0"
    }
    std::array<char, 32> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    out.append(buffer.data(), result.ptr);
}

} // namespace

std::optional<Path> Path::parse_svg(std::string_view data, std::string* error) {
    SvgPathParser parser{data};
    Path path;
    Vec2f current{}, start{};
    Vec2f last_control{};
    char last = 0;     // the previous command, for implicit repeats and smooth curves
    const auto fail = [&](const std::string& why) -> std::optional<Path> {
        if (error) {
            *error = why + " at " + std::to_string(parser.i);
        }
        return std::nullopt;
    };
    parser.skip();
    while (parser.i < data.size()) {
        char command = 0;
        if (std::isalpha(static_cast<unsigned char>(data[parser.i]))) {
            command = data[parser.i++];
        } else if (last != 0 && last != 'Z' && last != 'z') {
            // Numbers without a letter repeat the last command; after a move, as lines.
            command = last == 'M' ? 'L' : last == 'm' ? 'l' : last;
        } else {
            return fail("expected a command");
        }
        const bool relative = std::islower(static_cast<unsigned char>(command));
        const Vec2f origin = relative ? current : Vec2f{};
        const auto rel = [&](Vec2f p) { return Vec2f{p.x + origin.x, p.y + origin.y}; };
        const char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(command)));
        Vec2f p{}, c1{}, c2{};
        switch (upper) {
        case 'M':
            if (!parser.point(p)) return fail("M needs a point");
            current = start = rel(p);
            path.move_to(current);
            break;
        case 'L':
            if (!parser.point(p)) return fail("L needs a point");
            current = rel(p);
            path.line_to(current);
            break;
        case 'H':
            if (!parser.number(p.x)) return fail("H needs a number");
            current.x = relative ? current.x + p.x : p.x;
            path.line_to(current);
            break;
        case 'V':
            if (!parser.number(p.y)) return fail("V needs a number");
            current.y = relative ? current.y + p.y : p.y;
            path.line_to(current);
            break;
        case 'C':
            if (!parser.point(c1) || !parser.point(c2) || !parser.point(p)) return fail("C needs three points");
            c1 = rel(c1), c2 = rel(c2), p = rel(p);
            path.cubic_to(c1, c2, p);
            last_control = c2, current = p;
            break;
        case 'S': {
            if (!parser.point(c2) || !parser.point(p)) return fail("S needs two points");
            const char prev = static_cast<char>(std::toupper(static_cast<unsigned char>(last)));
            c1 = prev == 'C' || prev == 'S' ? Vec2f{2 * current.x - last_control.x, 2 * current.y - last_control.y} : current;
            c2 = rel(c2), p = rel(p);
            path.cubic_to(c1, c2, p);
            last_control = c2, current = p;
            break;
        }
        case 'Q':
            if (!parser.point(c1) || !parser.point(p)) return fail("Q needs two points");
            c1 = rel(c1), p = rel(p);
            path.quad_to(c1, p);
            last_control = c1, current = p;
            break;
        case 'T': {
            if (!parser.point(p)) return fail("T needs a point");
            const char prev = static_cast<char>(std::toupper(static_cast<unsigned char>(last)));
            c1 = prev == 'Q' || prev == 'T' ? Vec2f{2 * current.x - last_control.x, 2 * current.y - last_control.y} : current;
            p = rel(p);
            path.quad_to(c1, p);
            last_control = c1, current = p;
            break;
        }
        case 'A': {
            Vec2f radii{};
            f32 rotation = 0.0f;
            bool large = false, sweep = false;
            if (!parser.point(radii) || !parser.number(rotation) || !parser.flag(large) || !parser.flag(sweep) ||
                !parser.point(p)) {
                return fail("A needs radii, a rotation, two flags and a point");
            }
            p = rel(p);
            path.arc_to(radii, rotation, large, sweep, p);
            current = p;
            break;
        }
        case 'Z':
            path.close();
            current = start;
            break;
        default:
            return fail(std::string("unknown command '") + command + "'");
        }
        last = command;
        parser.skip();
        if (upper == 'Z' && parser.at_number()) {
            return fail("numbers after Z");
        }
    }
    return path;
}

std::string Path::to_svg() const {
    std::string out;
    std::size_t at = 0;
    const auto point = [&] {
        const Vec2f p = _points[at++];
        append_number(out, p.x);
        out += ' ';
        append_number(out, p.y);
    };
    for (const Verb verb : _verbs) {
        if (!out.empty()) {
            out += ' ';
        }
        switch (verb) {
        case Verb::Move: out += 'M'; point(); break;
        case Verb::Line: out += 'L'; point(); break;
        case Verb::Quad: out += 'Q'; point(); out += ' '; point(); break;
        case Verb::Cubic: out += 'C'; point(); out += ' '; point(); out += ' '; point(); break;
        case Verb::Close: out += 'Z'; break;
        }
    }
    return out;
}

} // namespace kin
