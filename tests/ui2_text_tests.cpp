// ui2 text beyond ASCII: atlases that grow, fallback fonts, right-to-left and
// shaped runs, CJK line breaking, the 5x7 font's accents.

#include <kin/platform/log.hpp>
#include <kin/renderer/backend.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <kin/ui2/text.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace kin;

class FakeTexture final : public ITextureBackend {
public:
    explicit FakeTexture(Vec2i size) : _size(size) {}
    Vec2i size() const override { return _size; }

private:
    Vec2i _size{};
};

struct Draw {
    Texture texture;
    Rectf source{};
    Rectf dest{};
};

// Makes textures (without pixels) and records what is drawn from them. It
// cannot update part of a texture, as the software backend cannot.
class TextureBackend final : public IRenderer2DBackend {
public:
    std::vector<Draw> draws;
    i32 textures_made = 0;
    u64 last_pixels = 0; // a hash of the last texture's pixels

    std::string_view name() const override { return "texture-recording"; }
    void clear(Color) override {}
    void present() override {}
    void set_logical_size(Vec2i) override {}
    void set_integer_logical_size(Vec2i) override {}
    Vec2i output_size() const override { return {640, 360}; }
    Vec2f window_to_logical(Vec2f v) const override { return v; }
    Vec2f logical_to_window(Vec2f v) const override { return v; }
    Texture create_texture_from_rgba(const u8* pixels, Vec2i size) override {
        ++textures_made;
        last_pixels = 1469598103934665603ull;
        for (std::size_t i = 0; pixels && i < static_cast<std::size_t>(size.x * size.y * 4); ++i) {
            last_pixels = (last_pixels ^ pixels[i]) * 1099511628211ull;
        }
        return Texture{std::make_shared<FakeTexture>(size)};
    }
    void draw_texture(const Texture& texture, Rectf dest) override { draws.push_back({texture, {}, dest}); }
    void draw_texture(const Texture& texture, Rectf source, Rectf dest) override {
        draws.push_back({texture, source, dest});
    }
    void fill_rect(Rectf, Color) override {}
    void draw_rect(Rectf, Color) override {}
    void draw_line(Vec2f, Vec2f, Color) override {}
    void set_viewport(Rectf) override {}
    void reset_viewport() override {}
    void push_viewport(Rectf) override {}
    void pop_viewport() override {}
};

struct Recording {
    TextureBackend* backend = nullptr;
    Renderer2D renderer;
};

Recording make_recording() {
    auto backend = std::make_unique<TextureBackend>();
    TextureBackend* raw = backend.get();
    return {raw, Renderer2D{std::move(backend)}};
}

using V = std::vector<std::string>;

// UTF-8 for the code points spelled out below, so the source stays plain.
std::string u8s(std::initializer_list<u32> cps) {
    std::string out;
    for (u32 c : cps) {
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

const std::string nihongo = u8s({0x65E5, 0x672C, 0x8A9E});  // 日本語
const std::string maru = u8s({0x3002});                     // 。
const std::string kagi = u8s({0x300C});                     // 「
const std::string privet = u8s({0x41F, 0x440, 0x438, 0x432, 0x435, 0x442}); // Привет
const std::string shalom = u8s({0x5E9, 0x5DC, 0x5D5, 0x5DD}); // שלום
const std::string salam = u8s({0x633, 0x644, 0x627, 0x645});  // سلام

void test_bitmap_font_accents() {
    const ui2::Font font = ui2::bitmap_font();
    assert(ui2::measure_text(font, "Gr\xC3\xBC\xC3\x9F" "e").x == ui2::measure_text(font, "Grube").x); // Grüße
    assert(ui2::measure_text(font, nihongo).x == ui2::measure_text(font, "abc").x); // a character each

    Recording rec = make_recording();
    ui2::draw_text(rec.renderer, font, "\xC3\xA9", {0, 0}, 1.0f, colors::white); // é, drawn as e
    assert(rec.backend->draws.size() == 1);
}

void test_cjk_wrapping() {
    // The 5x7 font: every character 6 wide (less 1 at the end), so the breaks
    // depend on nothing but the rules.
    const ui2::Font font = ui2::bitmap_font();
    // Three characters fit, four do not (17 wide; 15 summed per character as the ranges sum them).
    const ui2::TextWrapOptions three{.max_width = 17.0f};
    assert((ui2::wrap_text(font, nihongo + nihongo, three) == V{nihongo, nihongo}));

    // 。 never starts a line: it goes with the character before it.
    const std::string ni = u8s({0x65E5}), hon = u8s({0x672C}), go = u8s({0x8A9E});
    const V kinsoku = ui2::wrap_text(font, nihongo + maru + ni + hon, three);
    assert((kinsoku == V{ni + hon, go + maru + ni, hon}));
    // 「 never ends one.
    const V opening = ui2::wrap_text(font, ni + hon + kagi + go, three);
    assert((opening == V{ni + hon, kagi + go}));

    // Words keep their spaces; CJK next to Latin may break without one.
    assert((ui2::wrap_text(font, "ab " + nihongo, ui2::TextWrapOptions{.max_width = 23.0f}) == V{"ab " + ni, hon + go}));
    assert((ui2::wrap_text(font, "ab" + nihongo, three) == V{"ab" + ni, hon + go}));
    assert((ui2::wrap_text(font, "abcd " + nihongo, three) == V{"abc", "d", nihongo}));

    // The editor's ranges break the same way.
    const std::string text = nihongo + maru + ni + hon;
    const auto ranges = ui2::wrap_text_ranges(font, text, three);
    V lines;
    for (const ui2::TextRange& range : ranges) {
        lines.push_back(text.substr(range.begin, range.end - range.begin));
    }
    assert((lines == V{ni + hon, go + maru + ni, hon}));

    // A long word of several-byte characters breaks between characters, never inside one.
    const V long_word = ui2::wrap_text(font, privet, ui2::TextWrapOptions{.max_width = 11.0f});
    assert(long_word.size() == 3 && long_word[0] == privet.substr(0, 4));
}

// The system's UI font, if it has one, without fallbacks.
std::filesystem::path plain_system_font() {
    for (const char* path : {"C:/Windows/Fonts/segoeui.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                             "/Library/Fonts/Arial.ttf"}) {
        if (std::filesystem::exists(path)) {
            return path;
        }
    }
    return {};
}

ui2::FontSource cjk_fallback() {
    for (const ui2::FontSource& font : ui2::system_fallback_fonts()) {
        const std::string name = font.path.filename().string();
        for (const char* cjk : {"CJK", "msyh", "YuGoth", "meiryo", "Droid", "PingFang", "Hiragino"}) {
            if (name.find(cjk) != std::string::npos) {
                return font;
            }
        }
    }
    return {};
}

void test_atlas_grows_with_new_characters() {
    const std::filesystem::path path = plain_system_font();
    if (path.empty()) {
        return;
    }
    for (const ui2::TextRendering rendering : {ui2::TextRendering::Bitmap, ui2::TextRendering::Sdf}) {
        const ui2::Font font = ui2::load_ttf_font(path, 16.0f, ui2::TtfFontOptions{.rendering = rendering});
        const Vec2f size = ui2::measure_text(font, privet);
        assert(size.x > ui2::measure_text(font, "Pr").x && size.y > 0.0f);

        Recording rec = make_recording();
        ui2::draw_text(rec.renderer, font, privet, {10, 20}, 1.0f, colors::white);
        // One quad a letter, from an atlas, left to right.
        assert(rec.backend->draws.size() == 6);
        for (std::size_t i = 1; i < rec.backend->draws.size(); ++i) {
            assert(rec.backend->draws[i].dest.x > rec.backend->draws[i - 1].dest.x);
            assert(rec.backend->draws[i].texture == rec.backend->draws[0].texture);
        }
        // The same text again makes no texture.
        const i32 made = rec.backend->textures_made;
        rec.backend->draws.clear();
        ui2::draw_text(rec.renderer, font, privet + " Privet", {10, 20}, 1.0f, colors::white);
        assert(rec.backend->textures_made == made && rec.backend->draws.size() == 12);
    }
}

void test_fallback_fonts() {
    const std::filesystem::path path = plain_system_font();
    const ui2::FontSource cjk = cjk_fallback();
    if (path.empty() || cjk.path.empty()) {
        return;
    }
    const ui2::Font plain = ui2::load_ttf_font(path, 16.0f);
    const ui2::Font with_cjk = ui2::load_ttf_font(path, 16.0f, ui2::TtfFontOptions{.fallbacks = {cjk}});
    // Ideographs are an em wide each; the missing-glyph box is narrower.
    const f32 wide = ui2::measure_text(with_cjk, nihongo).x;
    assert(wide > 16.0f * 2.5f);
    assert(wide > ui2::measure_text(plain, nihongo).x);
    // Latin text is the font's own either way.
    assert(ui2::measure_text(with_cjk, "Latin").x == ui2::measure_text(plain, "Latin").x);

    Recording rec = make_recording();
    ui2::draw_text(rec.renderer, with_cjk, "A" + nihongo, {0, 0}, 1.0f, colors::white);
    assert(rec.backend->draws.size() == 4); // glyph by glyph, the fallback's glyphs in the same atlas
}

void test_right_to_left_runs() {
    const std::filesystem::path path = plain_system_font();
    if (path.empty()) {
        return;
    }
    const ui2::Font font = ui2::load_ttf_font(path, 16.0f, ui2::TtfFontOptions{.fallbacks = ui2::system_fallback_fonts()});
    assert(ui2::measure_text(font, shalom).x > 0.0f);
    assert(ui2::measure_text(font, salam).x > 0.0f);
    const f32 mixed = ui2::measure_text(font, "abc " + shalom).x;
    assert(mixed > ui2::measure_text(font, "abc").x + ui2::measure_text(font, shalom).x * 0.9f);

    // Left to right: a, b, c and the space from the atlas, then the Hebrew word
    // shaped as one texture, to their right.
    Recording rec = make_recording();
    ui2::draw_text(rec.renderer, font, "abc " + shalom, {0, 0}, 1.0f, colors::white);
    auto& draws = rec.backend->draws;
    assert(draws.size() == 4);
    const Draw hebrew = draws.back();
    assert(hebrew.dest.x > draws[2].dest.x);

    // A right-to-left paragraph puts the Hebrew word first, on the left.
    ui2::set_text_base_direction(TextDirection::RightToLeft);
    draws.clear();
    ui2::draw_text(rec.renderer, font, "abc " + shalom, {0, 0}, 1.0f, colors::white);
    assert(draws.size() == 4);
    // The glyphs share the atlas; the shaped run (the space joins it) has its own texture.
    const Texture atlas = draws[0].texture == draws[1].texture ? draws[0].texture : draws[2].texture;
    const auto shaped = std::ranges::find_if(draws, [&](const Draw& d) { return d.texture != atlas; });
    assert(shaped != draws.end() && std::ranges::count(draws, atlas, &Draw::texture) == 3);
    for (const Draw& d : draws) {
        if (d.texture == atlas) {
            assert(d.dest.x > shaped->dest.x);
        }
    }
    ui2::set_text_base_direction(std::nullopt);
    assert(!ui2::text_base_direction());

    // "3 سلام" right to left: the word on the left, then the space, then the 3.
    ui2::set_text_base_direction(TextDirection::RightToLeft);
    draws.clear();
    ui2::draw_text(rec.renderer, font, "3 " + salam, {0, 0}, 1.0f, colors::white);
    assert(draws.size() == 2);
    assert(draws[1].dest.x > draws[0].dest.x + draws[0].dest.w + 2.0f); // a space's width between
    ui2::set_text_base_direction(std::nullopt);

    // Arabic letters join: shaped as one run, drawn once.
    draws.clear();
    ui2::draw_text(rec.renderer, font, salam, {0, 0}, 1.0f, colors::white);
    assert(draws.size() == 1);

    // Two lines measure two lines high.
    const Vec2f one = ui2::measure_text(font, shalom);
    const Vec2f two = ui2::measure_text(font, shalom + "\n" + shalom);
    assert(two.y > one.y * 1.8f && std::abs(two.x - one.x) < 1e-3f);
}

// Text too wide is cut with an ellipsis or drawn smaller.
void test_fitting_text() {
    const ui2::Font font = ui2::bitmap_font(); // 6 units a character, less 1
    const auto fit = [&](std::string_view text, f32 width, ui2::TextFit how) {
        return ui2::fit_text(font, text, width, 1.0f, how);
    };
    const ui2::FittedText same = fit("Hello", 40.0f, ui2::TextFit::Ellipsis);
    assert(same.text == "Hello" && !same.changed && same.scale == 1.0f);
    assert(fit("Hello world", 30.0f, ui2::TextFit::Ellipsis).text == "He...");
    assert(fit("Hello world", 47.0f, ui2::TextFit::Ellipsis).text == "Hello..."); // the space before "..." goes
    // Shrinking as far as allowed (0.7), then cutting.
    const ui2::FittedText shrunk = fit("Hello world", 60.0f, ui2::TextFit::Shrink);
    assert(shrunk.text == "Hello world" && shrunk.scale < 1.0f && ui2::measure_text(font, shrunk.text, shrunk.scale).x <= 60.0f);
    const ui2::FittedText cut = fit("Hello world", 30.0f, ui2::TextFit::Shrink);
    assert(cut.scale == 0.7f && cut.text == "Hell..." && cut.changed);
    // Characters are never split.
    assert(fit(nihongo + nihongo, 23.0f, ui2::TextFit::Ellipsis).text == u8s({0x65E5}) + "...");

    // A TTF font ends with a real ellipsis.
    const std::filesystem::path path = plain_system_font();
    if (!path.empty()) {
        const ui2::Font ttf = ui2::load_ttf_font(path, 16.0f);
        const ui2::FittedText real = ui2::fit_text(ttf, "A rather long label", 60.0f, 1.0f, ui2::TextFit::Ellipsis);
        assert(real.text.ends_with("\xE2\x80\xA6") && ui2::measure_text(ttf, real.text).x <= 60.0f);
    }
}

// Fonts follow the text language: a Japanese and a Chinese face draw the same
// ideograph differently.
void test_language_fonts() {
    const std::vector<ui2::LanguageFont>& fonts = ui2::system_language_fonts();
    const auto font_for = [&](std::string_view language) -> const ui2::LanguageFont* {
        const auto found = std::ranges::find(fonts, language, &ui2::LanguageFont::language);
        return found == fonts.end() ? nullptr : &*found;
    };
    const std::filesystem::path path = plain_system_font();
    const ui2::LanguageFont* ja = font_for("ja");
    const ui2::LanguageFont* zh = font_for("zh");
    if (path.empty() || !ja || !zh) {
        return;
    }
    // Collections hold a face per language.
    if (ja->font.path == zh->font.path) {
        assert(ja->font.face != zh->font.face);
    }
    assert(fonts.front().language.size() >= fonts.back().language.size()); // particular tags first

    const ui2::Font font = ui2::load_ttf_font(path, 32.0f, ui2::TtfFontOptions{.language_fallbacks = fonts});
    Recording rec = make_recording();
    const std::string nao = u8s({0x76F4}); // 直: its strokes differ between Japanese and Chinese
    const auto pixels_in = [&](std::string_view language) {
        ui2::set_text_language(language);
        ui2::draw_text(rec.renderer, font, nao, {0, 0}, 1.0f, colors::white);
        return rec.backend->last_pixels;
    };
    const u64 japanese = pixels_in("ja-JP");
    const u64 chinese = pixels_in("zh-Hans");
    assert(japanese != chinese);
    assert(pixels_in("ja") == japanese);
    assert(ui2::text_language() == "ja");
    ui2::set_text_language({});
}

} // namespace

int main() {
    std::vector<LogEvent> log_events;
    set_logger_config({
        .min_level = LogLevel::Warn,
        .format = LogFormat::Text,
        .sdl_sink = false,
        .memory_events = &log_events,
    });
    test_bitmap_font_accents();
    test_cjk_wrapping();
    test_atlas_grows_with_new_characters();
    test_fallback_fonts();
    test_right_to_left_runs();
    test_fitting_text();
    test_language_fonts();
    return 0;
}
