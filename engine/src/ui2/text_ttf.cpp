#include "text_internal.hpp"

#include <kin/core/bidi.hpp>
#include <kin/core/jobs.hpp>
#include <kin/core/utf8.hpp>
#include <kin/platform/log.hpp>
#include <kin/renderer/renderer2d.hpp>

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../assets/content_io.hpp"

namespace kin::ui2 {
namespace {

// Opens a font from the content (a mounted pack, or disk), face `source.face`.
TTF_Font* open_font(const FontSource& source, f32 size) {
    SDL_IOStream* stream = open_content_stream(source.path);
    if (!stream) {
        return nullptr;
    }
    const SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, TTF_PROP_FONT_CREATE_IOSTREAM_POINTER, stream);
    SDL_SetBooleanProperty(props, TTF_PROP_FONT_CREATE_IOSTREAM_AUTOCLOSE_BOOLEAN, true); // closed with the font, or on failure
    SDL_SetFloatProperty(props, TTF_PROP_FONT_CREATE_SIZE_FLOAT, size);
    if (source.face != 0) {
        SDL_SetNumberProperty(props, TTF_PROP_FONT_CREATE_FACE_NUMBER, source.face);
    }
    TTF_Font* font = TTF_OpenFontWithProperties(props);
    SDL_DestroyProperties(props);
    return font;
}

using text_detail::kernable;
using text_detail::needs_shaping;

// ---- Signed distance fields for Sdf fonts ----

// One line of Felzenszwalb and Huttenlocher's exact squared Euclidean distance
// transform: d[q] = min over p of (q - p)^2 + f[p].
void distance_line(const f32* f, f32* d, i32* v, f32* z, i32 n) {
    constexpr f32 inf = 1e20f;
    i32 k = 0;
    v[0] = 0;
    z[0] = -inf;
    z[1] = inf;
    const auto meet = [&](i32 q, i32 p) {
        return ((f[q] + static_cast<f32>(q * q)) - (f[p] + static_cast<f32>(p * p))) / static_cast<f32>(2 * q - 2 * p);
    };
    for (i32 q = 1; q < n; ++q) {
        f32 s = meet(q, v[k]);
        while (s <= z[k]) {
            --k;
            s = meet(q, v[k]);
        }
        ++k;
        v[k] = q;
        z[k] = s;
        z[k + 1] = inf;
    }
    k = 0;
    for (i32 q = 0; q < n; ++q) {
        while (z[k + 1] < static_cast<f32>(q)) {
            ++k;
        }
        d[q] = static_cast<f32>((q - v[k]) * (q - v[k])) + f[v[k]];
    }
}

// The squared distance from every cell to the nearest one marked, down the
// columns then along the rows.
std::vector<f32> squared_distance(const std::vector<u8>& marked, i32 w, i32 h) {
    std::vector<f32> grid(marked.size());
    for (std::size_t i = 0; i < marked.size(); ++i) {
        grid[i] = marked[i] ? 0.0f : 1e20f;
    }
    const i32 n = std::max(w, h);
    std::vector<f32> f(static_cast<std::size_t>(n)), d(static_cast<std::size_t>(n)), z(static_cast<std::size_t>(n) + 1);
    std::vector<i32> v(static_cast<std::size_t>(n));
    for (i32 x = 0; x < w; ++x) {
        for (i32 y = 0; y < h; ++y) f[static_cast<std::size_t>(y)] = grid[static_cast<std::size_t>(y * w + x)];
        distance_line(f.data(), d.data(), v.data(), z.data(), h);
        for (i32 y = 0; y < h; ++y) grid[static_cast<std::size_t>(y * w + x)] = d[static_cast<std::size_t>(y)];
    }
    for (i32 y = 0; y < h; ++y) {
        std::copy_n(grid.begin() + y * w, w, f.begin());
        distance_line(f.data(), d.data(), v.data(), z.data(), w);
        std::copy_n(d.begin(), w, grid.begin() + y * w);
    }
    return grid;
}

// A glyph's coverage (`w` x `h` RGBA, drawn `supersample` times larger than
// the field) as a signed distance field: `spread` field pixels of margin all
// round, each texel's alpha 0.5 on the outline and 0.5 more or less `spread`
// pixels inside or out.
struct DistanceField {
    i32 w = 0;
    i32 h = 0;
    std::vector<u8> alpha;
};

DistanceField distance_field(const u8* rgba, i32 pitch, i32 w, i32 h, i32 supersample, f32 spread) {
    const i32 k = supersample;
    const i32 pad = static_cast<i32>(spread) * k;
    const i32 cw = (w + 2 * pad + k - 1) / k, ch = (h + 2 * pad + k - 1) / k; // the field's size
    const i32 hw = cw * k, hh = ch * k;
    std::vector<u8> inside(static_cast<std::size_t>(hw * hh), 0), outside(inside.size(), 1);
    for (i32 y = 0; y < h; ++y) {
        for (i32 x = 0; x < w; ++x) {
            if (rgba[y * pitch + x * 4 + 3] >= 128) {
                const std::size_t i = static_cast<std::size_t>((y + pad) * hw + x + pad);
                inside[i] = 1;
                outside[i] = 0;
            }
        }
    }
    const std::vector<f32> to_inside = squared_distance(inside, hw, hh);
    const std::vector<f32> to_outside = squared_distance(outside, hw, hh);
    DistanceField field{cw, ch, std::vector<u8>(static_cast<std::size_t>(cw * ch))};
    for (i32 fy = 0; fy < ch; ++fy) {
        for (i32 fx = 0; fx < cw; ++fx) {
            // The block's mean signed distance (pixel centres half a pixel off
            // the boundary), in field pixels.
            f32 sum = 0.0f;
            for (i32 y = fy * k; y < fy * k + k; ++y) {
                for (i32 x = fx * k; x < fx * k + k; ++x) {
                    const std::size_t i = static_cast<std::size_t>(y * hw + x);
                    sum += inside[i] ? std::sqrt(to_outside[i]) - 0.5f : 0.5f - std::sqrt(to_inside[i]);
                }
            }
            const f32 d = sum / static_cast<f32>(k * k * k);
            const f32 a = std::clamp(0.5f + d / (2.0f * spread), 0.0f, 1.0f);
            field.alpha[static_cast<std::size_t>(fy * cw + fx)] = static_cast<u8>(a * 255.0f + 0.5f);
        }
    }
    return field;
}

void ensure_ttf() {
    static const bool initialized = [] {
        if (!TTF_Init()) {
            KIN_LOG_ERROR_F("ui",
                            "ttf init failed",
                            (LogFields{{.name = "error", .value = SDL_GetError()}}));
            throw std::runtime_error(std::string{"TTF_Init failed: "} + SDL_GetError());
        }
        KIN_LOG_INFO("ui", "ttf initialized");
        return true;
    }();
    (void)initialized;
}

// Text drawn white (to be tinted), tightly packed RGBA.
struct Image {
    Vec2i size{};
    std::vector<u8> rgba;
};

Image render_white(TTF_Font* font, std::string_view text) {
    Image image;
    SDL_Surface* surface = TTF_RenderText_Blended(font, text.data(), text.size(), SDL_Color{255, 255, 255, 255});
    if (!surface) {
        return image;
    }
    SDL_Surface* converted = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(surface);
    if (!converted) {
        return image;
    }
    image.size = {converted->w, converted->h};
    image.rgba.resize(static_cast<std::size_t>(converted->w * converted->h * 4));
    const auto* src = static_cast<const u8*>(converted->pixels);
    for (int y = 0; y < converted->h; ++y) {
        std::copy_n(src + y * converted->pitch, converted->w * 4,
                    image.rgba.begin() + static_cast<std::ptrdiff_t>(y * converted->w * 4));
    }
    SDL_DestroySurface(converted);
    return image;
}

Image render_glyph(TTF_Font* font, u32 c) {
    std::string text;
    utf8_append(text, c);
    return render_white(font, text);
}

// ---- Laying out text from a face's metrics ----

constexpr i32 first_glyph = 32; // ' '
constexpr i32 glyph_count = 95; // ' ' to '~'

bool printable(u32 c) { return c >= static_cast<u32>(first_glyph) && c < static_cast<u32>(first_glyph + glyph_count); }

// A character of two to four bytes at `k`, moving `k` past it; malformed
// bytes as utf8_decode and utf8_next take them.
u32 decode_multibyte(std::string_view text, std::size_t& k) {
    const auto lead = static_cast<unsigned char>(text[k]);
    const std::size_t length = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC2 ? 2 : 0;
    if (length != 0 && length <= 4 && k + length <= text.size()) {
        u32 c = lead & (0x7Fu >> length);
        bool ok = true;
        for (std::size_t i = 1; i < length; ++i) {
            const auto byte = static_cast<unsigned char>(text[k + i]);
            ok = ok && (byte & 0xC0u) == 0x80u;
            c = (c << 6) | (byte & 0x3Fu);
        }
        const bool shortest = length == 2 || (length == 3 && c >= 0x800 && (c < 0xD800 || c > 0xDFFF)) ||
                              (length == 4 && c >= 0x10000 && c <= 0x10FFFF);
        if (ok && shortest) {
            k += length;
            return c;
        }
    }
    const u32 c = utf8_decode(text, k);
    k = utf8_next(text, k);
    return c;
}

// The code point at `k`, moving `k` past it; ASCII without decoding.
inline u32 next_code_point(std::string_view text, std::size_t& k) {
    const auto byte = static_cast<unsigned char>(text[k]);
    if (byte < 0x80) {
        ++k;
        return byte;
    }
    return decode_multibyte(text, k);
}

// Values by code point: the Basic Multilingual Plane in pages of 256 made as
// they are first written, the rest in a map.
template <typename T>
class CodePointTable {
public:
    const T* find(u32 c) const {
        if (c < 0x10000) {
            const Page* page = _pages[c >> 8].get();
            return page && page->known[c & 0xFF] ? &page->values[c & 0xFF] : nullptr;
        }
        const auto it = _far.find(c);
        return it != _far.end() ? &it->second : nullptr;
    }
    bool contains(u32 c) const { return find(c) != nullptr; }
    // The value for `c`, made (default) if there was none.
    T& at(u32 c) {
        if (c < 0x10000) {
            std::unique_ptr<Page>& page = _pages[c >> 8];
            if (!page) {
                page = std::make_unique<Page>();
            }
            page->known[c & 0xFF] = true;
            return page->values[c & 0xFF];
        }
        return _far[c];
    }

private:
    struct Page {
        std::array<T, 256> values{};
        std::array<bool, 256> known{};
    };
    std::array<std::unique_ptr<Page>, 256> _pages{};
    std::unordered_map<u32, T> _far;
};

// A font's kerning in ems, as SDL_ttf shapes text with HarfBuzz: GPOS
// kerning as well as a legacy 'kern' table. (TTF_GetGlyphKerning reads only
// the legacy table, which most current fonts leave empty.) SDL_ttf has no call
// for a pair's kerning, so it is solved from the width SDL_ttf measures for
// the two glyphs together, at a large size for precision. Printable ASCII is a
// table whose rows are solved the first time their glyph leads a pair; other
// pairs below the CJK blocks are solved one by one as they appear. One table
// serves every size of the font. Its face also answers which characters the
// font itself has (its sizes may borrow from fallbacks).
class KerningTable {
public:
    explicit KerningTable(std::filesystem::path path) : _path(std::move(path)) {}
    KerningTable(const KerningTable&) = delete;
    KerningTable& operator=(const KerningTable&) = delete;
    ~KerningTable() {
        if (_face) {
            TTF_CloseFont(_face);
        }
    }

    f32 em(u32 previous, u32 ch) {
        if (printable(previous) && printable(ch)) [[likely]] {
            const auto row = static_cast<std::size_t>(previous) - first_glyph;
            if (!_solved[row]) [[unlikely]] {
                solve_row(row);
            }
            return _em[row * glyph_count + static_cast<std::size_t>(ch) - first_glyph];
        }
        return em_beyond_ascii(previous, ch);
    }

    f32 em_beyond_ascii(u32 previous, u32 ch) {
        if (!kernable(previous) || !kernable(ch)) {
            return 0.0f;
        }
        const u64 key = (static_cast<u64>(previous) << 32) | ch;
        const auto [it, inserted] = _pairs.try_emplace(key, 0.0f);
        if (inserted) {
            it->second = solve_pair(previous, ch);
        }
        return it->second;
    }

    // Whether the font has `ch` itself. Printable ASCII is taken as given.
    bool has(u32 ch) {
        if (ch < 0x80 || !open()) {
            return true;
        }
        const auto [it, inserted] = _has.try_emplace(ch, false);
        if (inserted) {
            it->second = TTF_FontHasGlyph(_face, ch);
        }
        return it->second;
    }

private:
    static constexpr f32 size = 512.0f;

    struct Ink {
        int min_x = 0;
        int max_x = 0;
        int advance = 0;
        bool found = false;
    };

    bool open() {
        if (_face || _failed) {
            return _face != nullptr;
        }
        _face = open_font({_path, 0}, size);
        if (!_face) {
            _failed = true;
            return false;
        }
        for (i32 i = 0; i < glyph_count; ++i) {
            _ink[static_cast<std::size_t>(i)] = read_ink(static_cast<u32>(i + first_glyph));
        }
        return true;
    }

    Ink read_ink(u32 ch) const {
        Ink ink;
        int min_y = 0;
        int max_y = 0;
        ink.found = TTF_GetGlyphMetrics(_face, ch, &ink.min_x, &ink.max_x, &min_y, &max_y, &ink.advance);
        return ink;
    }

    const Ink& ink(u32 ch) {
        if (printable(ch)) {
            return _ink[static_cast<std::size_t>(ch) - first_glyph];
        }
        const auto [it, inserted] = _other_ink.try_emplace(ch);
        if (inserted) {
            it->second = has(ch) ? read_ink(ch) : Ink{};
        }
        return it->second;
    }

    // The kerning between a and b, in ems; 0 when it cannot be told.
    f32 solve(u32 first, u32 second, const Ink& a, const Ink& b) const {
        if (!a.found || !b.found) {
            return 0.0f;
        }
        std::string pair;
        utf8_append(pair, first);
        utf8_append(pair, second);
        int w = 0;
        int h = 0;
        if (!TTF_GetStringSize(_face, pair.data(), pair.size(), &w, &h)) {
            return 0.0f;
        }
        // SDL_ttf measures from the leftmost ink (or the pen's start) to
        // the rightmost ink (or the pen's end); b's pen is a's advance
        // plus the kerning.
        const int left = std::min(0, a.min_x);
        const int kerning = w + left - a.advance - std::max(b.max_x, b.advance);
        if (a.advance + kerning + b.min_x < left) {
            return 0.0f; // b's ink would start the measure: the width says nothing of the kerning
        }
        return static_cast<f32>(kerning) / size;
    }

    void solve_row(std::size_t row) {
        _solved[row] = true;
        if (!open()) {
            return;
        }
        const u32 first = static_cast<u32>(row) + first_glyph;
        for (std::size_t col = 0; col < glyph_count; ++col) {
            const u32 second = static_cast<u32>(col) + first_glyph;
            _em[row * glyph_count + col] = solve(first, second, _ink[row], _ink[col]);
        }
    }

    f32 solve_pair(u32 first, u32 second) {
        if (!open() || !has(first) || !has(second)) {
            return 0.0f;
        }
        return solve(first, second, ink(first), ink(second));
    }

    std::filesystem::path _path;
    TTF_Font* _face = nullptr;
    bool _failed = false;
    std::array<Ink, glyph_count> _ink{};
    std::array<bool, glyph_count> _solved{};
    std::array<f32, glyph_count * glyph_count> _em{};
    std::unordered_map<u64, f32> _pairs;
    std::unordered_map<u32, Ink> _other_ink;
    std::unordered_map<u32, bool> _has;
};

// Everything about a face that lays out text, read once per glyph: each
// glyph's advance and ink, the face it comes from (the font or a fallback)
// and the line's height, with the font's kerning scaled to the face. Drawing
// and measuring then never ask SDL_ttf per glyph.
struct GlyphMetrics {
    struct Glyph {
        f32 advance = 0.0f;    // the pen's move; 0: no glyph (moves as a space)
        f32 ink_left = 0.0f;   // where the ink starts, from the pen
        f32 ink_right = 0.0f;  // and where it ends
        f32 ink_top = 0.0f;    // from the top of the line, down
        f32 ink_bottom = 0.0f;
        f32 y_offset = 0.0f;   // a fallback's image, down to the font's baseline
        u8 face = 0;           // 0: the font; n: fallback n - 1
    };
    std::array<Glyph, 128> ascii{};
    CodePointTable<Glyph> others; // read as text needs them
    f32 font_height = 0.0f; // a line's height before any ink beyond it
    f32 line_step = 0.0f;   // from one line to the next
    f32 pixels_per_em = 0.0f;
    f32 ascent = 0.0f;
    KerningTable* kerning = nullptr;
    // Printable ASCII pairs' kerning in this face's pixels, a row filled the
    // first time its glyph leads a pair: the walk reads it with no call.
    mutable std::array<f32, glyph_count * glyph_count> ascii_kerning{};
    mutable std::array<bool, glyph_count> ascii_rows{};

    const Glyph* find(u32 c) const { return c < 128 ? &ascii[c] : others.find(c); }

    f32 ascii_kern(u32 previous, u32 ch) const {
        const auto row = static_cast<std::size_t>(previous) - first_glyph;
        if (!ascii_rows[row]) [[unlikely]] {
            fill_row(row);
        }
        return ascii_kerning[row * glyph_count + ch - first_glyph];
    }

    void fill_row(std::size_t row) const {
        ascii_rows[row] = true;
        for (std::size_t col = 0; col < glyph_count; ++col) {
            ascii_kerning[row * glyph_count + col] =
                kerning ? kerning->em(static_cast<u32>(row) + first_glyph, static_cast<u32>(col) + first_glyph) *
                              pixels_per_em
                        : 0.0f;
        }
    }

    f32 kern(u32 previous, u32 ch, const Glyph& a, const Glyph& b) const {
        if (!kerning || (a.face | b.face) != 0) {
            return 0.0f;
        }
        return kerning->em(previous, ch) * pixels_per_em;
    }
};

GlyphMetrics read_glyph_metrics(TTF_Font* font, f32 pixels_per_em, KerningTable* kerning) {
    GlyphMetrics metrics;
    metrics.font_height = static_cast<f32>(std::max(1, TTF_GetFontHeight(font)));
    metrics.line_step = metrics.font_height;
    metrics.pixels_per_em = pixels_per_em;
    metrics.kerning = kerning;
    metrics.ascent = static_cast<f32>(TTF_GetFontAscent(font));
    for (char ch = first_glyph; ch < first_glyph + glyph_count; ++ch) {
        const std::string text{ch};
        int extent_w = 0;
        int extent_h = 0;
        if (!TTF_GetStringSize(font, text.c_str(), text.size(), &extent_w, &extent_h)) {
            continue;
        }
        // The pen moves by the glyph's advance, not by the width of its
        // bitmap: a glyph that overhangs its advance (italics, an f or a j)
        // would otherwise push the next one away.
        int min_x = 0;
        int max_x = extent_w;
        int min_y = 0;
        int max_y = 0;
        int advance = extent_w;
        if (!TTF_GetGlyphMetrics(font, static_cast<Uint32>(ch), &min_x, &max_x, &min_y, &max_y, &advance)) {
            min_x = 0;
            max_x = extent_w;
            advance = extent_w;
        }
        metrics.ascii[static_cast<std::size_t>(ch)] = {
            .advance = static_cast<f32>(std::max(1, advance)),
            .ink_left = static_cast<f32>(min_x),
            .ink_right = static_cast<f32>(max_x),
            .ink_top = metrics.ascent - static_cast<f32>(max_y),
            .ink_bottom = metrics.ascent - static_cast<f32>(min_y),
        };
        metrics.line_step = std::max(metrics.line_step, static_cast<f32>(extent_h));
    }
    return metrics;
}

// How text is laid out.
enum class TextKind : u8 {
    Ascii,  // printable ASCII: the glyph walk, from the face's ASCII metrics
    Simple, // other characters a glyph walk places (metrics read as they appear)
    Shaped, // something needs the bidirectional algorithm or a shaper
};

TextKind classify(std::string_view text) {
    // Most UI text, checked first in a loop the compiler can vectorize.
    const bool ascii = std::ranges::all_of(text, [](char ch) {
        const auto value = static_cast<unsigned char>(ch);
        return (value >= first_glyph && value < first_glyph + glyph_count) || value == '\n' || value == '\r' ||
               value == '\t';
    });
    if (ascii) {
        return TextKind::Ascii;
    }
    TextKind kind = TextKind::Ascii;
    for (std::size_t k = 0; k < text.size();) {
        const auto byte = static_cast<unsigned char>(text[k]);
        if (byte < 0x80) {
            if (byte < first_glyph && byte != '\n' && byte != '\r' && byte != '\t') {
                kind = TextKind::Simple; // a control character: drawn as nothing
            }
            ++k;
            continue;
        }
        const u32 c = next_code_point(text, k);
        if (needs_shaping(c)) {
            return TextKind::Shaped;
        }
        kind = TextKind::Simple;
    }
    return kind;
}

// Text laid out as SDL_ttf lays out a line: the pen moves by each glyph's
// advance and the kerning before it; '\n' starts a line, '\t' is four spaces
// and '\r' nothing. `visit(c, pen, glyph)` is called for each glyph, the pen in
// face pixels from the text's origin (the top of its first line). Every
// character's metrics must have been read (TtfFontBackend::ensure_metrics).
// Returns the size SDL_ttf measures a line: from the leftmost ink, or the
// pen's start, to the rightmost ink or the pen's end, and from the line's top
// to its bottom unless ink reaches beyond them. So text draws as wide as it
// measures. `end_pen`, when given, receives where the pen ends on the last line.
// `Ascii`: the text is printable ASCII (TextKind::Ascii), walked without decoding.
template <bool Ascii, typename Visit>
Vec2f walk_glyphs(const GlyphMetrics& metrics, std::string_view text, Visit&& visit, f32* end_pen = nullptr) {
    static const GlyphMetrics::Glyph none{};
    const GlyphMetrics::Glyph& space = metrics.ascii[static_cast<std::size_t>(' ')];
    const f32 space_advance = space.advance > 0.0f ? space.advance : metrics.font_height * 0.5f;
    struct Bounds {
        f32 left = 0.0f;
        f32 top = 0.0f;
        f32 right = 0.0f;
        f32 bottom = 0.0f;
    };
    Vec2f pen{0.0f, 0.0f};
    Bounds line;  // this line's ink, or its pen and height where they reach further
    Bounds text_; // every line's
    u32 previous = 0;
    const GlyphMetrics::Glyph* previous_glyph = &none;
    const auto start_line = [&] {
        line = {.left = 0.0f, .top = pen.y, .right = 0.0f, .bottom = pen.y + metrics.font_height};
    };
    const auto end_line = [&] {
        text_.right = std::max(text_.right, std::max(line.right, pen.x) - line.left);
        text_.top = std::min(text_.top, line.top);
        text_.bottom = std::max(text_.bottom, line.bottom);
    };
    start_line();
    for (std::size_t k = 0; k < text.size();) {
        u32 c = 0;
        if constexpr (Ascii) {
            c = static_cast<unsigned char>(text[k++]);
        } else {
            c = next_code_point(text, k);
        }
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            end_line();
            pen = {0.0f, pen.y + metrics.line_step};
            start_line();
            previous = 0;
            continue;
        }
        if (c == '\t') {
            pen.x += space_advance * 4.0f;
            previous = 0;
            continue;
        }
        const GlyphMetrics::Glyph* found = Ascii ? &metrics.ascii[c] : metrics.find(c);
        const GlyphMetrics::Glyph& glyph = found ? *found : none;
        if (previous != 0) {
            if constexpr (Ascii) {
                pen.x += metrics.ascii_kern(previous, c);
            } else {
                pen.x += printable(previous) && printable(c) ? metrics.ascii_kern(previous, c)
                                                             : metrics.kern(previous, c, *previous_glyph, glyph);
            }
        }
        previous = c;
        previous_glyph = &glyph;
        if (glyph.advance > 0.0f) {
            line.left = std::min(line.left, pen.x + glyph.ink_left);
            line.top = std::min(line.top, pen.y + glyph.ink_top);
            line.right = std::max(line.right, pen.x + glyph.ink_right);
            line.bottom = std::max(line.bottom, pen.y + glyph.ink_bottom);
        }
        visit(c, pen, glyph);
        pen.x += glyph.advance > 0.0f ? glyph.advance : space_advance;
    }
    end_line();
    if (end_pen) {
        *end_pen = pen.x;
    }
    return {text_.right, text_.bottom - text_.top}; // text_.right: the widest line's width
}

// ---- Atlases that grow ----

// A texture glyphs are packed into, row by row, with a copy of its pixels so
// it can be made again where a backend cannot update part of a texture.
struct AtlasPage {
    Texture texture;
    Vec2i size{};
    std::vector<u8> pixels; // RGBA
    i32 x = 0;
    i32 y = 0;
    i32 row_height = 0;
    bool stale = false; // pixels changed that the texture lacks
};

constexpr i32 atlas_padding = 1;

// Room for a `size` image in `page`, at `at`; false if it is full.
bool pack(AtlasPage& page, Vec2i size, Vec2i& at) {
    const i32 w = size.x + atlas_padding * 2;
    const i32 h = size.y + atlas_padding * 2;
    if (w > page.size.x || h > page.size.y) {
        return false;
    }
    if (page.x + w > page.size.x) {
        page.y += page.row_height;
        page.x = 0;
        page.row_height = 0;
    }
    if (page.y + h > page.size.y) {
        return false;
    }
    at = {page.x + atlas_padding, page.y + atlas_padding};
    page.x += w;
    page.row_height = std::max(page.row_height, h);
    return true;
}

// Writes `rgba` (`size`, tightly packed) into the page at `at`, and into its
// texture when it has one.
void blit(Renderer2D& renderer, AtlasPage& page, Vec2i at, Vec2i size, const u8* rgba) {
    for (i32 y = 0; y < size.y; ++y) {
        std::copy_n(rgba + static_cast<std::ptrdiff_t>(y * size.x * 4), size.x * 4,
                    page.pixels.begin() + static_cast<std::ptrdiff_t>(((at.y + y) * page.size.x + at.x) * 4));
    }
    if (page.texture && !page.stale && !renderer.update_texture(page.texture, at, size, rgba)) {
        page.stale = true;
    }
}

AtlasPage make_page(Vec2i size, u8 fill_rgb) {
    AtlasPage page;
    page.size = size;
    page.pixels.assign(static_cast<std::size_t>(size.x * size.y * 4), 0);
    if (fill_rgb != 0) {
        for (std::size_t i = 0; i < page.pixels.size(); i += 4) {
            page.pixels[i] = page.pixels[i + 1] = page.pixels[i + 2] = fill_rgb;
        }
    }
    page.stale = true;
    return page;
}

// Makes the textures of pages whose pixels they lack.
void refresh_pages(Renderer2D& renderer, std::vector<AtlasPage>& pages, bool linear) {
    for (AtlasPage& page : pages) {
        if (!page.stale) {
            continue;
        }
        page.texture = renderer.create_texture_from_rgba(page.pixels.data(), page.size);
        if (page.texture && linear) {
            renderer.set_scale_mode(page.texture, ScaleMode::Linear);
        }
        page.stale = false;
    }
}

i32 page_side(i32 cell, i32 min_side, i32 max_side) {
    const i32 wanted = std::max(min_side, cell * 16);
    return std::clamp(static_cast<i32>(std::bit_ceil(static_cast<u32>(wanted))), min_side, max_side);
}

struct AtlasGlyph {
    Rectf source{};
    u8 page = 0;
    bool drawable = false;
};

// Glyphs by code point: printable ASCII in an array, the rest as they come.
struct AtlasGlyphs {
    std::array<AtlasGlyph, 128> ascii{};
    CodePointTable<AtlasGlyph> others;

    const AtlasGlyph* find(u32 c) const { return c < 128 ? &ascii[c] : others.find(c); }
    bool has(u32 c) const { return c < 128 || others.contains(c); }
    AtlasGlyph& at(u32 c) { return c < 128 ? ascii[c] : others.at(c); }
};

// The language text is shown in, and a count of its changes, which fonts
// watch to choose their language fallbacks again.
std::mutex g_language_mutex;
std::string g_language;
std::atomic<u64> g_language_generation{1};

std::string current_language() {
    const std::scoped_lock lock(g_language_mutex);
    return g_language;
}

// Whether `language` is `wanted` or a more particular tag of it ("zh-Hant-TW" of "zh-Hant").
bool language_matches(std::string_view language, std::string_view wanted) {
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c == '_' ? '-' : c; };
    if (wanted.empty() || language.size() < wanted.size()) {
        return false;
    }
    for (std::size_t i = 0; i < wanted.size(); ++i) {
        if (lower(language[i]) != lower(wanted[i])) {
            return false;
        }
    }
    return language.size() == wanted.size() || language[wanted.size()] == '-' || language[wanted.size()] == '_';
}

std::atomic<bool> g_has_base_direction{false};
std::atomic<TextDirection> g_base_direction{TextDirection::LeftToRight};

std::optional<TextDirection> base_direction() {
    if (!g_has_base_direction.load(std::memory_order_relaxed)) {
        return std::nullopt;
    }
    return g_base_direction.load(std::memory_order_relaxed);
}

class TtfFontBackend final : public IFontBackend {
public:
    TtfFontBackend(const std::filesystem::path& path, f32 point_size, const TtfFontOptions& options)
        : _path(path), _point_size(point_size), _oversample(std::max(1.0f, options.oversample)),
          _rendering(options.rendering), _kerning(path) {
        ensure_ttf();
        for (const FontSource& fallback : options.fallbacks) {
            if (!fallback.path.empty() && !(fallback.path == path && fallback.face == 0)) {
                _general_fallbacks.push_back(fallback);
            }
        }
        _language_fallbacks = options.language_fallbacks;
        refresh_language();
        if (!face_for_scale(1.0f)) {
            throw std::runtime_error(std::string{"TTF_OpenFont failed: "} + SDL_GetError());
        }
        KIN_LOG_INFO_F("ui",
                       "font loaded",
                       (LogFields{{.name = "path", .value = path.string()},
                                  {.name = "size", .value = std::to_string(point_size)},
                                  {.name = "fallbacks", .value = std::to_string(_fallbacks.size())}}));
    }

    ~TtfFontBackend() override {
        for (FontFace& face : _faces) {
            close(face);
        }
        close(_sdf);
        close_probes();
    }

    Vec2f measure(std::string_view text, f32 scale) const override {
        refresh_language();
        if (_rendering == TextRendering::Sdf) {
            // Linear: the large face's size, scaled down.
            FontFace* face = sdf_face();
            if (!face) {
                return {};
            }
            const f32 s = sdf_scale(scale) / static_cast<f32>(sdf_supersample);
            const Vec2f size = measure_face(*face, text);
            return {size.x * s, size.y * s};
        }
        FontFace* face = face_for_scale(scale);
        if (!face) {
            return {};
        }
        const Vec2f size = measure_face(*face, text);
        return {size.x / _oversample, size.y / _oversample};
    }

    bool draw_outlined(Renderer2D& renderer, std::string_view text, Vec2f pos, f32 scale, Color color, f32 outline,
                       Color outline_color) const override {
        refresh_language();
        return _rendering == TextRendering::Sdf && renderer.capabilities().distance_fields &&
               draw_distance_fields(renderer, text, pos, scale, color, outline, outline_color);
    }

    void draw(Renderer2D& renderer, std::string_view text, Vec2f pos, f32 scale, Color color) const override {
        if (text.empty()) {
            return;
        }
        refresh_language();
        if (_rendering == TextRendering::Sdf && renderer.capabilities().distance_fields &&
            draw_distance_fields(renderer, text, pos, scale, color, 0.0f, colors::transparent)) {
            return;
        }
        FontFace* face = face_for_scale(scale);
        if (!face) {
            return;
        }
        GlyphAtlas* atlas = find_glyph_atlas(renderer, *face, scale);
        const TextKind kind = classify(text);
        if (kind == TextKind::Shaped) {
            draw_layout(renderer, *face, atlas, text, pos, scale, color);
            return;
        }
        if (kind == TextKind::Simple) {
            ensure_metrics(*face, text);
            if (atlas) {
                add_glyphs(renderer, *face, *atlas, text);
            }
        }
        if (atlas) {
            if (kind == TextKind::Ascii) {
                draw_glyphs<true>(renderer, *face, *atlas, text, pos, color);
            } else {
                draw_glyphs<false>(renderer, *face, *atlas, text, pos, color);
            }
            return;
        }
        // No atlas could be made: the whole text as one texture.
        if (const CachedTextTexture* cached = shaped_texture(renderer, *face, text, false, scale, false)) {
            renderer.draw_texture(cached->texture, Rectf{0, 0, static_cast<f32>(cached->size.x), static_cast<f32>(cached->size.y)},
                                  {snap(pos.x), snap(pos.y), static_cast<f32>(cached->size.x) / _oversample,
                                   static_cast<f32>(cached->size.y) / _oversample},
                                  color);
        }
    }

private:
    struct Fallback {
        FontSource source;
        TTF_Font* probe = nullptr; // a small face, to ask which characters it has
        bool tried = false;
    };

    struct FontFace {
        f32 point_size = 0.0f;
        TTF_Font* font = nullptr;
        u64 last_used = 0;
        std::unique_ptr<GlyphMetrics> metrics; // read on first use
        std::vector<TTF_Font*> fallbacks;      // by Fallback, opened when first needed
        bool fallbacks_attached = false;       // all added to `font`, for shaping
        std::unordered_map<std::string, Vec2f> shaped_sizes; // 'L' or 'R' and the text
    };

    struct CachedTextTexture {
        u64 renderer_id = 0;
        std::string text;
        f32 scale = 1.0f;
        bool rtl = false;
        bool sdf = false;
        Texture texture;
        Vec2i size{};
        u64 last_used = 0;
    };

    struct GlyphAtlas {
        u64 renderer_id = 0;
        f32 scale = 1.0f;
        std::vector<AtlasPage> pages;
        AtlasGlyphs glyphs;
        u64 last_used = 0;
        bool full = false; // logged once
    };

    // Sdf: one atlas of distance fields at the base size, made from glyphs
    // drawn `sdf_supersample` times larger; laid out by the large face's
    // metrics, scaled.
    static constexpr f32 sdf_base = 48.0f;    // the field's em, in its pixels
    static constexpr f32 sdf_spread = 8.0f;   // field pixels of distance either side of the outline
    static constexpr i32 sdf_supersample = 2;
    static constexpr i32 sdf_page_side = 1024;

    struct SdfAtlas {
        u64 renderer_id = 0;
        std::vector<AtlasPage> pages;
        AtlasGlyphs glyphs;
        bool full = false;
    };

    // A run of a laid-out line, in face pixels.
    struct Segment {
        std::size_t begin = 0; // bytes of the text
        std::size_t end = 0;
        u32 line = 0;
        bool shaped = false; // drawn from a texture HarfBuzz shaped
        bool rtl = false;
        f32 x = 0.0f;
        f32 width = 0.0f;
    };
    struct Layout {
        std::vector<Segment> segments;
        Vec2f size{};
    };

    static constexpr std::size_t max_cache_entries = 512;
    // Bitmap: faces and atlases, one a size, kept for the most recent few.
    static constexpr std::size_t max_faces = 12;
    static constexpr std::size_t max_glyph_atlases = 8;
    static constexpr std::size_t max_atlas_pages = 4;
    static constexpr std::size_t max_shaped_sizes = 1024;

    static void close(FontFace& face) {
        if (face.font) {
            TTF_CloseFont(face.font);
            face.font = nullptr;
        }
        for (TTF_Font*& fallback : face.fallbacks) {
            if (fallback) {
                TTF_CloseFont(fallback);
                fallback = nullptr;
            }
        }
    }

    // Base pixels to drawing units at `scale`.
    f32 sdf_scale(f32 scale) const { return _point_size * std::max(0.0f, scale) / sdf_base; }

    // The large face the fields are drawn from: a pixel of it is
    // 1 / `sdf_supersample` of a base pixel.
    FontFace* sdf_face() const {
        if (!_sdf.font) {
            _sdf.point_size = sdf_base * static_cast<f32>(sdf_supersample);
            _sdf.font = open_font({_path, 0}, _sdf.point_size);
            set_language(_sdf.font);
        }
        return _sdf.font ? &_sdf : nullptr;
    }

    const GlyphMetrics& metrics_of(FontFace& face) const {
        if (!face.metrics) {
            // A face's point size is its pixels per em (SDL_ttf's 72 dpi).
            face.metrics = std::make_unique<GlyphMetrics>(read_glyph_metrics(face.font, face.point_size, &_kerning));
        }
        return *face.metrics;
    }

    // ---- Fallbacks ----

    // Shapes with the text language (forms that differ by language).
    void set_language(TTF_Font* font) const {
        if (font && !_language.empty()) {
            TTF_SetFontLanguage(font, _language.c_str());
        }
    }

    void close_probes() const {
        for (Fallback& fallback : _fallbacks) {
            if (fallback.probe) {
                TTF_CloseFont(fallback.probe);
                fallback.probe = nullptr;
            }
        }
    }

    // Follows set_text_language: the fallbacks for the language, before the
    // general ones. A change drops what was made from the old ones.
    void refresh_language() const {
        const u64 generation = g_language_generation.load(std::memory_order_acquire);
        if (generation == _language_generation) {
            return;
        }
        _language_generation = generation;
        _language = current_language();
        std::vector<FontSource> chain;
        for (const LanguageFont& font : _language_fallbacks) {
            if (language_matches(_language, font.language) && std::ranges::find(chain, font.font) == chain.end()) {
                chain.push_back(font.font);
            }
        }
        for (const FontSource& font : _general_fallbacks) {
            if (std::ranges::find(chain, font) == chain.end()) {
                chain.push_back(font);
            }
        }
        // Shaping depends on the language: shaped runs are made again.
        _cache.clear();
        for (FontFace* face : all_faces()) {
            face->shaped_sizes.clear();
            set_language(face->font);
        }
        const bool same = chain.size() == _fallbacks.size() &&
                          std::ranges::equal(chain, _fallbacks, {}, {}, &Fallback::source);
        if (same) {
            return;
        }
        // Other fallbacks: glyphs may come from other faces now.
        for (FontFace* face : all_faces()) {
            if (face->font) {
                TTF_ClearFallbackFonts(face->font);
            }
            for (TTF_Font*& fallback : face->fallbacks) {
                if (fallback) {
                    TTF_CloseFont(fallback);
                    fallback = nullptr;
                }
            }
            face->fallbacks.clear();
            face->fallbacks_attached = false;
            face->metrics.reset();
        }
        _glyph_atlases.clear();
        _sdf_atlases.clear();
        close_probes();
        _fallbacks.clear();
        for (const FontSource& font : chain) {
            _fallbacks.push_back({.source = font});
        }
    }

    std::vector<FontFace*> all_faces() const {
        std::vector<FontFace*> faces;
        for (FontFace& face : _faces) {
            faces.push_back(&face);
        }
        faces.push_back(&_sdf);
        return faces;
    }

    TTF_Font* probe(std::size_t i) const {
        Fallback& fallback = _fallbacks[i];
        if (!fallback.tried) {
            fallback.tried = true;
            fallback.probe = open_font(fallback.source, 16.0f);
            if (!fallback.probe) {
                KIN_LOG_WARN_F("ui", "fallback font open failed",
                               (LogFields{{.name = "path", .value = fallback.source.path.string()},
                                          {.name = "error", .value = SDL_GetError()}}));
            }
        }
        return fallback.probe;
    }

    // The face a character comes from: 0 the font, n fallback n - 1. A
    // character none has is the font's (its missing-glyph box).
    u8 owner_of(u32 c) const {
        if (c < 0x80 || _kerning.has(c)) {
            return 0;
        }
        for (std::size_t i = 0; i < _fallbacks.size() && i < 254; ++i) {
            if (TTF_Font* font = probe(i); font && TTF_FontHasGlyph(font, c)) {
                return static_cast<u8>(i + 1);
            }
        }
        return 0;
    }

    TTF_Font* fallback_font(FontFace& face, std::size_t i) const {
        face.fallbacks.resize(_fallbacks.size(), nullptr);
        if (!face.fallbacks[i] && probe(i)) {
            face.fallbacks[i] = open_font(_fallbacks[i].source, face.point_size);
            set_language(face.fallbacks[i]);
        }
        return face.fallbacks[i];
    }

    TTF_Font* font_of(FontFace& face, u8 owner) const {
        if (owner == 0) {
            return face.font;
        }
        TTF_Font* font = fallback_font(face, owner - 1u);
        return font ? font : face.font;
    }

    // Lets SDL_ttf (shaping whole runs) take glyphs from the fallbacks.
    void attach_fallbacks(FontFace& face) const {
        if (face.fallbacks_attached) {
            return;
        }
        face.fallbacks_attached = true;
        for (std::size_t i = 0; i < _fallbacks.size(); ++i) {
            if (TTF_Font* fallback = fallback_font(face, i)) {
                TTF_AddFallbackFont(face.font, fallback);
            }
        }
    }

    // ---- Metrics ----

    // Reads the metrics of the characters in `text` the face has not met.
    // Characters that are shaped are left to the shaper.
    void ensure_metrics(FontFace& face, std::string_view text) const {
        metrics_of(face);
        GlyphMetrics& metrics = *face.metrics;
        for (std::size_t k = 0; k < text.size();) {
            const auto byte = static_cast<unsigned char>(text[k]);
            if (byte < 0x80) {
                ++k;
                continue;
            }
            const u32 c = next_code_point(text, k);
            if (needs_shaping(c) || metrics.others.contains(c)) {
                continue;
            }
            const GlyphMetrics::Glyph glyph = read_glyph(face, metrics, c);
            metrics.others.at(c) = glyph;
        }
    }

    GlyphMetrics::Glyph read_glyph(FontFace& face, const GlyphMetrics& metrics, u32 c) const {
        u8 owner = owner_of(c);
        TTF_Font* font = font_of(face, owner);
        if (font == face.font) {
            owner = 0;
        }
        GlyphMetrics::Glyph glyph;
        glyph.face = owner;
        int min_x = 0;
        int max_x = 0;
        int min_y = 0;
        int max_y = 0;
        int advance = 0;
        if (TTF_GetGlyphMetrics(font, c, &min_x, &max_x, &min_y, &max_y, &advance)) {
            glyph.advance = static_cast<f32>(std::max(0, advance));
            glyph.ink_left = static_cast<f32>(min_x);
            glyph.ink_right = static_cast<f32>(max_x);
            glyph.ink_top = metrics.ascent - static_cast<f32>(max_y);
            glyph.ink_bottom = metrics.ascent - static_cast<f32>(min_y);
        }
        // A fallback's image starts at its own line's top: move it so the
        // baselines meet.
        glyph.y_offset = metrics.ascent - static_cast<f32>(TTF_GetFontAscent(font));
        return glyph;
    }

    // In the face's pixels.
    Vec2f measure_face(FontFace& face, std::string_view text) const {
        switch (classify(text)) {
        case TextKind::Ascii:
            return walk_glyphs<true>(metrics_of(face), text, [](u32, Vec2f, const GlyphMetrics::Glyph&) {});
        case TextKind::Simple:
            ensure_metrics(face, text);
            return walk_glyphs<false>(metrics_of(face), text, [](u32, Vec2f, const GlyphMetrics::Glyph&) {});
        case TextKind::Shaped:
            return layout(face, text).size;
        }
        return {};
    }

    // ---- Shaped text ----

    // A run's size when HarfBuzz shapes it in `face`, in its pixels.
    Vec2f shaped_size(FontFace& face, std::string_view run, bool rtl) const {
        std::string key;
        key.reserve(run.size() + 1);
        key += rtl ? 'R' : 'L';
        key += run;
        if (const auto it = face.shaped_sizes.find(key); it != face.shaped_sizes.end()) {
            return it->second;
        }
        attach_fallbacks(face);
        TTF_SetFontDirection(face.font, rtl ? TTF_DIRECTION_RTL : TTF_DIRECTION_LTR);
        int w = 0;
        int h = 0;
        const bool ok = TTF_GetStringSize(face.font, run.data(), run.size(), &w, &h);
        TTF_SetFontDirection(face.font, TTF_DIRECTION_INVALID);
        const Vec2f size = ok ? Vec2f{static_cast<f32>(w), static_cast<f32>(h)} : Vec2f{};
        if (face.shaped_sizes.size() >= max_shaped_sizes) {
            face.shaped_sizes.clear();
        }
        face.shaped_sizes.emplace(std::move(key), size);
        return size;
    }

    // Lines split at '\n', each ordered by the bidirectional algorithm into
    // runs: those that run right to left or hold characters that need a
    // shaper are shaped whole; the others are walked glyph by glyph.
    Layout layout(FontFace& face, std::string_view text) const {
        ensure_metrics(face, text);
        const GlyphMetrics& metrics = metrics_of(face);
        const std::optional<TextDirection> base = base_direction();
        Layout out;
        f32 bottom = 0.0f;
        std::size_t start = 0;
        for (u32 line = 0;; ++line) {
            const std::size_t newline = text.find('\n', start);
            const std::size_t end = newline == std::string_view::npos ? text.size() : newline;
            std::string_view line_text = text.substr(start, end - start);
            if (line_text.ends_with('\r')) {
                line_text.remove_suffix(1);
            }
            f32 x = 0.0f;
            f32 height = metrics.font_height;
            const auto add = [&](std::size_t begin, std::size_t end, bool shaped, bool rtl) {
                if (begin == end) {
                    return;
                }
                const std::string_view part = line_text.substr(begin, end - begin);
                Segment segment{.begin = start + begin, .end = start + end, .line = line, .shaped = shaped, .rtl = rtl, .x = x};
                if (shaped) {
                    const Vec2f size = shaped_size(face, part, rtl);
                    segment.width = size.x;
                    height = std::max(height, size.y);
                } else {
                    walk_glyphs<false>(metrics, part, [](u32, Vec2f, const GlyphMetrics::Glyph&) {}, &segment.width);
                }
                x += segment.width;
                out.segments.push_back(segment);
            };
            for (const BidiRun& run : bidi_runs(line_text, base)) {
                const std::string_view part = line_text.substr(run.begin, run.end - run.begin);
                bool shaped = run.right_to_left();
                for (std::size_t k = 0; !shaped && k < part.size();) {
                    shaped = needs_shaping(next_code_point(part, k));
                }
                if (!shaped) {
                    add(run.begin, run.end, false, false);
                    continue;
                }
                // Spaces at a shaped run's ends are laid out here, on the side
                // the run's direction puts them: a shaper may not keep them.
                std::size_t lead = 0;
                while (lead < part.size() && part[lead] == ' ') ++lead;
                std::size_t trail = 0;
                while (trail < part.size() - lead && part[part.size() - 1 - trail] == ' ') ++trail;
                const std::size_t core_begin = run.begin + lead;
                const std::size_t core_end = run.end - trail;
                const bool rtl = run.right_to_left();
                add(rtl ? core_end : run.begin, rtl ? run.end : core_begin, false, false);
                add(core_begin, core_end, true, rtl);
                add(rtl ? run.begin : core_end, rtl ? core_begin : run.end, false, false);
            }
            out.size.x = std::max(out.size.x, x);
            bottom = static_cast<f32>(line) * metrics.line_step + height;
            if (newline == std::string_view::npos) {
                break;
            }
            start = newline + 1;
        }
        out.size.y = bottom;
        return out;
    }

    // A run shaped and drawn white by SDL_ttf, cached. `sdf`: from the large
    // face, to be drawn scaled.
    const CachedTextTexture* shaped_texture(Renderer2D& renderer, FontFace& face, std::string_view run, bool rtl,
                                            f32 scale, bool sdf) const {
        ++_cache_tick;
        const f32 key_scale = sdf ? 0.0f : scale;
        auto found = std::ranges::find_if(_cache, [&](const CachedTextTexture& entry) {
            return entry.renderer_id == renderer.id() && entry.rtl == rtl && entry.sdf == sdf &&
                   same_scale(entry.scale, key_scale) && entry.text == run;
        });
        if (found != _cache.end()) {
            found->last_used = _cache_tick;
            return &*found;
        }
        attach_fallbacks(face);
        TTF_SetFontDirection(face.font, rtl ? TTF_DIRECTION_RTL : TTF_DIRECTION_LTR);
        const Image image = render_white(face.font, run);
        TTF_SetFontDirection(face.font, TTF_DIRECTION_INVALID);
        if (image.rgba.empty()) {
            return nullptr;
        }
        Texture texture = renderer.create_texture_from_rgba(image.rgba.data(), image.size);
        if (!texture) {
            return nullptr;
        }
        if (sdf) {
            renderer.set_scale_mode(texture, ScaleMode::Linear);
        }
        if (_cache.size() >= max_cache_entries) {
            _cache.erase(std::ranges::min_element(_cache, {}, &CachedTextTexture::last_used));
        }
        _cache.push_back(CachedTextTexture{
            .renderer_id = renderer.id(),
            .text = std::string{run},
            .scale = key_scale,
            .rtl = rtl,
            .sdf = sdf,
            .texture = std::move(texture),
            .size = image.size,
            .last_used = _cache_tick,
        });
        return &_cache.back();
    }

    // ---- Bitmap ----

    static f32 effective_point_size(f32 point_size, f32 scale) {
        return std::max(1.0f, point_size * std::max(0.0f, scale));
    }

    static bool same_scale(f32 a, f32 b) {
        return std::abs(a - b) < 0.001f;
    }

    // Rounds to the grid the glyphs were rasterized for: logical pixels
    // normally, output pixels when oversampled, so every atlas texel lands on
    // exactly one pixel either way.
    f32 snap(f32 v) const { return std::round(v * _oversample) / _oversample; }

    FontFace* face_for_scale(f32 scale) const {
        const f32 size = effective_point_size(_point_size * _oversample, scale);
        ++_face_tick;
        auto found = std::ranges::find_if(_faces, [&](const FontFace& face) {
            return same_scale(face.point_size, size);
        });
        if (found != _faces.end()) {
            found->last_used = _face_tick;
            return &*found;
        }
        if (_faces.size() >= max_faces) {
            // A scale that keeps changing (an animation) would open faces
            // without end: the least recently used goes.
            const auto oldest = std::ranges::min_element(_faces, {}, &FontFace::last_used);
            close(*oldest);
            _faces.erase(oldest);
        }

        TTF_Font* font = open_font({_path, 0}, size);
        if (!font) {
            KIN_LOG_ERROR_F("ui",
                            "font open failed",
                            (LogFields{{.name = "path", .value = _path.string()},
                                       {.name = "size", .value = std::to_string(size)},
                                       {.name = "error", .value = SDL_GetError()}}));
            return nullptr;
        }
        set_language(font);
        _faces.push_back({.point_size = size, .font = font, .last_used = _face_tick});
        return &_faces.back();
    }

    GlyphAtlas* find_glyph_atlas(Renderer2D& renderer, FontFace& face, f32 scale) const {
        ++_cache_tick;
        auto found = std::ranges::find_if(_glyph_atlases, [&](const GlyphAtlas& atlas) {
            return atlas.renderer_id == renderer.id() && same_scale(atlas.scale, scale);
        });
        if (found != _glyph_atlases.end()) {
            found->last_used = _cache_tick;
            return &*found;
        }

        if (GlyphAtlas atlas = build_glyph_atlas(renderer, face, scale); !atlas.pages.empty() && atlas.pages[0].texture) {
            atlas.last_used = _cache_tick;
            if (_glyph_atlases.size() >= max_glyph_atlases) {
                _glyph_atlases.erase(std::ranges::min_element(_glyph_atlases, {}, &GlyphAtlas::last_used));
            }
            _glyph_atlases.push_back(std::move(atlas));
            return &_glyph_atlases.back();
        }
        return nullptr;
    }

    // An atlas with printable ASCII, on a page with room for more.
    GlyphAtlas build_glyph_atlas(Renderer2D& renderer, FontFace& face, f32 scale) const {
        GlyphAtlas atlas;
        atlas.renderer_id = renderer.id();
        atlas.scale = scale;

        std::vector<std::pair<u32, Image>> images;
        images.reserve(glyph_count);
        i32 cell = 1;
        // ' ' has no pixels; its advance is in the face's GlyphMetrics.
        for (u32 ch = first_glyph + 1; ch < static_cast<u32>(first_glyph + glyph_count); ++ch) {
            Image image = render_glyph(face.font, ch);
            if (image.rgba.empty()) {
                continue;
            }
            cell = std::max({cell, image.size.x, image.size.y});
            images.emplace_back(ch, std::move(image));
        }
        if (images.empty()) {
            return {};
        }
        const i32 side = page_side(cell + atlas_padding * 2, 256, 4096);
        atlas.pages.push_back(make_page({side, side}, 0));
        for (const auto& [ch, image] : images) {
            Vec2i at;
            if (!pack(atlas.pages[0], image.size, at)) {
                continue;
            }
            blit(renderer, atlas.pages[0], at, image.size, image.rgba.data());
            atlas.glyphs.ascii[ch] = {.source = {static_cast<f32>(at.x), static_cast<f32>(at.y),
                                                 static_cast<f32>(image.size.x), static_cast<f32>(image.size.y)},
                                      .page = 0,
                                      .drawable = true};
        }
        refresh_pages(renderer, atlas.pages, false);
        return atlas;
    }

    // Packs the glyphs of `text` the atlas lacks (not those that are shaped).
    void add_glyphs(Renderer2D& renderer, FontFace& face, GlyphAtlas& atlas, std::string_view text) const {
        const GlyphMetrics& metrics = metrics_of(face);
        bool added = false;
        for (std::size_t k = 0; k < text.size();) {
            const auto byte = static_cast<unsigned char>(text[k]);
            if (byte < 0x80) {
                ++k;
                continue;
            }
            const u32 c = next_code_point(text, k);
            if (atlas.glyphs.has(c) || needs_shaping(c)) {
                continue;
            }
            AtlasGlyph& glyph = atlas.glyphs.at(c);
            const GlyphMetrics::Glyph* source = metrics.find(c);
            if (!source || source->advance <= 0.0f) {
                continue; // nothing to draw
            }
            const Image image = render_glyph(font_of(face, source->face), c);
            if (image.rgba.empty()) {
                continue;
            }
            Vec2i at;
            if (!place(atlas.pages, image.size, at, glyph.page, 0, atlas.full)) {
                continue;
            }
            blit(renderer, atlas.pages[glyph.page], at, image.size, image.rgba.data());
            glyph.source = {static_cast<f32>(at.x), static_cast<f32>(at.y), static_cast<f32>(image.size.x),
                            static_cast<f32>(image.size.y)};
            glyph.drawable = true;
            added = true;
        }
        if (added) {
            refresh_pages(renderer, atlas.pages, false);
        }
    }

    // Room for an image on the last page, or a new one; false when the atlas
    // has all the pages it may.
    static bool place(std::vector<AtlasPage>& pages, Vec2i size, Vec2i& at, u8& page, u8 fill_rgb, bool& full) {
        if (!pages.empty() && pack(pages.back(), size, at)) {
            page = static_cast<u8>(pages.size() - 1);
            return true;
        }
        if (pages.size() >= max_atlas_pages) {
            if (!full) {
                full = true;
                KIN_LOG_WARN("ui", "glyph atlas full: further characters are not drawn");
            }
            return false;
        }
        const Vec2i side = pages.empty() ? Vec2i{1024, 1024} : pages.back().size;
        pages.push_back(make_page(side, fill_rgb));
        if (!pack(pages.back(), size, at)) {
            return false;
        }
        page = static_cast<u8>(pages.size() - 1);
        return true;
    }

    // Draws glyph-walked text from the atlas, the pen starting at `origin`
    // (drawing units) and the walk in face pixels.
    template <bool Ascii>
    void draw_glyphs(Renderer2D& renderer, FontFace& face, const GlyphAtlas& atlas, std::string_view text,
                     Vec2f origin, Color color) const {
        const f32 inv = 1.0f / _oversample;
        const GlyphMetrics& metrics = metrics_of(face);
        walk_glyphs<Ascii>(metrics, text, [&](u32 ch, Vec2f pen, const GlyphMetrics::Glyph& source) {
            const AtlasGlyph* glyph = atlas.glyphs.find(ch);
            if (!glyph || !glyph->drawable) {
                return;
            }
            // The bitmap starts at the ink when that is left of the pen (SDL_ttf
            // renders a string from min(0, its first glyph's left)). Pixel-snap
            // the destination: the glyph atlas is integer-sized, so an integer
            // dest is a 1:1 blit (crisp). Fractional positions (from accumulated
            // sub-pixel advances) make the GPU linear-sample off-grid, which
            // softens and unevens the text. The fractional pen is kept for
            // correct average advance; only the draw position rounds.
            const f32 left = std::min(0.0f, source.ink_left);
            renderer.draw_texture(atlas.pages[glyph->page].texture,
                                  glyph->source,
                                  {snap(origin.x + (pen.x + left) * inv),
                                   snap(origin.y + (pen.y + source.y_offset) * inv),
                                   glyph->source.w * inv,
                                   glyph->source.h * inv},
                                  color);
        });
    }

    // Bitmap text that needs the bidirectional algorithm or a shaper.
    void draw_layout(Renderer2D& renderer, FontFace& face, GlyphAtlas* atlas, std::string_view text, Vec2f pos,
                     f32 scale, Color color) const {
        const Layout laid = layout(face, text);
        const GlyphMetrics& metrics = metrics_of(face);
        if (atlas) {
            add_glyphs(renderer, face, *atlas, text);
        }
        const f32 inv = 1.0f / _oversample;
        for (const Segment& segment : laid.segments) {
            const std::string_view part = text.substr(segment.begin, segment.end - segment.begin);
            const Vec2f origin{pos.x + segment.x * inv, pos.y + static_cast<f32>(segment.line) * metrics.line_step * inv};
            if (!segment.shaped) {
                if (atlas) {
                    draw_glyphs<false>(renderer, face, *atlas, part, origin, color);
                }
                continue;
            }
            if (const CachedTextTexture* cached = shaped_texture(renderer, face, part, segment.rtl, scale, false)) {
                renderer.draw_texture(cached->texture,
                                      Rectf{0, 0, static_cast<f32>(cached->size.x), static_cast<f32>(cached->size.y)},
                                      {snap(origin.x), snap(origin.y), static_cast<f32>(cached->size.x) * inv,
                                       static_cast<f32>(cached->size.y) * inv},
                                      color);
            }
        }
    }

    // ---- Sdf ----

    SdfAtlas* find_sdf_atlas(Renderer2D& renderer) const {
        for (SdfAtlas& atlas : _sdf_atlases) {
            if (atlas.renderer_id == renderer.id()) {
                return &atlas;
            }
        }
        SdfAtlas atlas;
        atlas.renderer_id = renderer.id();
        std::vector<u32> ascii;
        // ' ' has no pixels; its advance is in the face's GlyphMetrics.
        for (u32 ch = first_glyph + 1; ch < static_cast<u32>(first_glyph + glyph_count); ++ch) {
            ascii.push_back(ch);
        }
        add_sdf_glyphs(renderer, atlas, ascii);
        if (atlas.pages.empty() || !atlas.pages[0].texture) {
            return nullptr;
        }
        _sdf_atlases.push_back(std::move(atlas));
        return &_sdf_atlases.back();
    }

    // Draws `chars` large, makes their distance fields on the job system's
    // workers, and packs them.
    void add_sdf_glyphs(Renderer2D& renderer, SdfAtlas& atlas, const std::vector<u32>& chars) const {
        FontFace* large = sdf_face();
        if (!large || chars.empty()) {
            return;
        }
        const GlyphMetrics& metrics = metrics_of(*large);
        struct Made {
            u32 ch = 0;
            Image image;
            DistanceField field;
        };
        std::vector<Made> made;
        made.reserve(chars.size());
        // SDL_ttf wants one thread.
        for (const u32 ch : chars) {
            const GlyphMetrics::Glyph* source = metrics.find(ch);
            Image image = render_glyph(font_of(*large, source ? source->face : 0), ch);
            if (!image.rgba.empty()) {
                made.push_back({ch, std::move(image), {}});
            }
        }
        default_job_system().parallel_for(static_cast<i32>(made.size()), [&](i32 i) {
            Made& m = made[static_cast<std::size_t>(i)];
            m.field = distance_field(m.image.rgba.data(), m.image.size.x * 4, m.image.size.x, m.image.size.y,
                                     sdf_supersample, sdf_spread);
            m.image = {};
        });
        std::vector<u8> rgba;
        bool added = false;
        for (const Made& m : made) {
            const DistanceField& field = m.field;
            AtlasGlyph& glyph = atlas.glyphs.at(m.ch);
            Vec2i at;
            if (field.w <= 0 || !place_sdf(atlas, {field.w, field.h}, at, glyph.page)) {
                continue;
            }
            rgba.assign(static_cast<std::size_t>(field.w * field.h * 4), 255);
            for (std::size_t i = 0; i < field.alpha.size(); ++i) {
                rgba[i * 4 + 3] = field.alpha[i];
            }
            blit(renderer, atlas.pages[glyph.page], at, {field.w, field.h}, rgba.data());
            glyph.source = {static_cast<f32>(at.x), static_cast<f32>(at.y), static_cast<f32>(field.w),
                            static_cast<f32>(field.h)};
            glyph.drawable = true;
            added = true;
        }
        if (added) {
            refresh_pages(renderer, atlas.pages, true); // fields are read between texels
        }
    }

    bool place_sdf(SdfAtlas& atlas, Vec2i size, Vec2i& at, u8& page) const {
        if (atlas.pages.empty()) {
            atlas.pages.push_back(make_page({sdf_page_side, sdf_page_side}, 255));
        }
        return place(atlas.pages, size, at, page, 255, atlas.full);
    }

    // The fields of the characters in `text` the atlas lacks.
    void ensure_sdf_glyphs(Renderer2D& renderer, SdfAtlas& atlas, std::string_view text) const {
        std::vector<u32> missing;
        for (std::size_t k = 0; k < text.size();) {
            const auto byte = static_cast<unsigned char>(text[k]);
            if (byte < 0x80) {
                ++k;
                continue;
            }
            const u32 c = next_code_point(text, k);
            if (!atlas.glyphs.has(c) && !needs_shaping(c) && std::ranges::find(missing, c) == missing.end()) {
                missing.push_back(c);
                atlas.glyphs.at(c); // tried, drawable or not
            }
        }
        add_sdf_glyphs(renderer, atlas, missing);
    }

    bool draw_distance_fields(Renderer2D& renderer, std::string_view text, Vec2f pos, f32 scale, Color color,
                              f32 outline, Color outline_color) const {
        SdfAtlas* atlas = find_sdf_atlas(renderer);
        FontFace* face = sdf_face();
        if (!atlas || !face) {
            return false;
        }
        const f32 s = sdf_scale(scale);
        if (s <= 0.0f) {
            return true;
        }
        const TextKind kind = classify(text);
        if (kind != TextKind::Ascii) {
            ensure_metrics(*face, text);
            ensure_sdf_glyphs(renderer, *atlas, text);
        }
        const f32 face_s = s / static_cast<f32>(sdf_supersample); // the large face's pixels to drawing units
        const GlyphMetrics& metrics = metrics_of(*face);
        thread_local std::vector<std::vector<SpriteInstance>> quads; // by page
        quads.resize(std::max(quads.size(), atlas->pages.size()));
        for (auto& page : quads) {
            page.clear();
        }
        const auto add_quads = [&](std::string_view part, Vec2f origin) {
            walk_glyphs<false>(metrics, part, [&](u32 ch, Vec2f pen, const GlyphMetrics::Glyph& source) {
                const AtlasGlyph* glyph = atlas->glyphs.find(ch);
                if (!glyph || !glyph->drawable) {
                    return;
                }
                // As the Bitmap atlas: the glyph's box starts at its ink when that
                // is left of the pen, and its field reaches `sdf_spread` beyond.
                const f32 left = std::min(0.0f, source.ink_left);
                quads[glyph->page].push_back({.dest = {origin.x + (pen.x + left) * face_s - sdf_spread * s,
                                                       origin.y + (pen.y + source.y_offset) * face_s - sdf_spread * s,
                                                       glyph->source.w * s, glyph->source.h * s},
                                              .source = glyph->source,
                                              .tint = color});
            });
        };
        std::vector<std::pair<const CachedTextTexture*, Rectf>> shaped;
        if (kind == TextKind::Shaped) {
            for (const Segment& segment : layout(*face, text).segments) {
                const std::string_view part = text.substr(segment.begin, segment.end - segment.begin);
                const Vec2f origin{pos.x + segment.x * face_s,
                                   pos.y + static_cast<f32>(segment.line) * metrics.line_step * face_s};
                if (!segment.shaped) {
                    add_quads(part, origin);
                } else if (const CachedTextTexture* cached = shaped_texture(renderer, *face, part, segment.rtl, scale, true)) {
                    shaped.emplace_back(cached, Rectf{origin.x, origin.y, static_cast<f32>(cached->size.x) * face_s,
                                                      static_cast<f32>(cached->size.y) * face_s});
                }
            }
        } else {
            add_quads(text, pos);
        }
        // The outline, in drawing units, as field pixels (at most what the
        // field holds outside the glyph).
        const DistanceFieldStyle field{.spread = sdf_spread,
                                       .outline = outline_color.a > 0 ? std::min(outline / s, sdf_spread - 1.0f) : 0.0f,
                                       .outline_color = outline_color};
        bool drawn = true;
        for (std::size_t page = 0; page < atlas->pages.size(); ++page) {
            if (!quads[page].empty()) {
                drawn = renderer.draw_distance_field(atlas->pages[page].texture, quads[page], field) && drawn;
            }
        }
        // Shaped runs are plain textures: their outline is stamped round them.
        for (const auto& [cached, dest] : shaped) {
            const Rectf source{0, 0, static_cast<f32>(cached->size.x), static_cast<f32>(cached->size.y)};
            if (outline > 0.0f && outline_color.a > 0) {
                for (const Vec2f off : {Vec2f{-outline, 0}, Vec2f{outline, 0}, Vec2f{0, -outline}, Vec2f{0, outline}}) {
                    renderer.draw_texture(cached->texture, source, {dest.x + off.x, dest.y + off.y, dest.w, dest.h},
                                          outline_color);
                }
            }
            renderer.draw_texture(cached->texture, source, dest, color);
        }
        return drawn;
    }

    std::filesystem::path _path;
    f32 _point_size = 0.0f;
    f32 _oversample = 1.0f;
    TextRendering _rendering = TextRendering::Bitmap;
    std::vector<FontSource> _general_fallbacks;
    std::vector<LanguageFont> _language_fallbacks;
    mutable std::vector<Fallback> _fallbacks; // the chain for the text language
    mutable std::string _language;
    mutable u64 _language_generation = 0;
    mutable FontFace _sdf; // Sdf: the large face, opened on first use
    mutable KerningTable _kerning;
    mutable std::vector<SdfAtlas> _sdf_atlases;
    mutable u64 _face_tick = 0;
    mutable std::vector<FontFace> _faces;
    mutable std::vector<GlyphAtlas> _glyph_atlases;
    mutable std::vector<CachedTextTexture> _cache;
    mutable u64 _cache_tick = 0;
};

} // namespace

void set_text_language(std::string_view language) {
    {
        const std::scoped_lock lock(g_language_mutex);
        if (g_language == language) {
            return;
        }
        g_language = language;
    }
    g_language_generation.fetch_add(1, std::memory_order_acq_rel);
}

std::string text_language() {
    return current_language();
}

void set_text_base_direction(std::optional<TextDirection> direction) {
    if (direction) {
        g_base_direction.store(*direction, std::memory_order_relaxed);
    }
    g_has_base_direction.store(direction.has_value(), std::memory_order_relaxed);
}

std::optional<TextDirection> text_base_direction() {
    return base_direction();
}

namespace text_detail {

std::optional<i32> find_font_face(const std::filesystem::path& path, std::string_view family) {
    ensure_ttf();
    for (i32 face = 0; face < 16; ++face) {
        TTF_Font* font = open_font({path, face}, 12.0f);
        if (!font) {
            return std::nullopt;
        }
        const char* name = TTF_GetFontFamilyName(font);
        const bool match = family.empty() || (name && std::string_view{name}.find(family) != std::string_view::npos);
        const int faces = TTF_GetNumFontFaces(font);
        TTF_CloseFont(font);
        if (match) {
            return face;
        }
        if (face + 1 >= faces) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

std::shared_ptr<const IFontBackend> make_ttf_backend(const std::filesystem::path& path, f32 point_size,
                                                     const TtfFontOptions& options) {
    return std::make_shared<TtfFontBackend>(path, point_size, options);
}

} // namespace text_detail

} // namespace kin::ui2
