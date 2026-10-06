// ui2 text on the CPU: a HUD's worth of labels measured and drawn each frame,
// in ASCII, accented Latin and Cyrillic, through a backend that only records.
// Prints microseconds a frame for each, the median of several runs.
//
//   cmake --build build --target kin_text_bench && ./build/bin/kin_text_bench

#include <kin/renderer/backend.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <kin/ui2/text.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace kin;

class NullTexture final : public ITextureBackend {
public:
    explicit NullTexture(Vec2i size) : _size(size) {}
    Vec2i size() const override { return _size; }

private:
    Vec2i _size{};
};

class CountingBackend final : public IRenderer2DBackend {
public:
    u64 draws = 0;
    std::string_view name() const override { return "counting"; }
    void clear(Color) override {}
    void present() override {}
    void set_logical_size(Vec2i) override {}
    void set_integer_logical_size(Vec2i) override {}
    Vec2i output_size() const override { return {1920, 1080}; }
    Vec2f window_to_logical(Vec2f v) const override { return v; }
    Vec2f logical_to_window(Vec2f v) const override { return v; }
    Texture create_texture_from_rgba(const u8*, Vec2i size) override { return Texture{std::make_shared<NullTexture>(size)}; }
    void draw_texture(const Texture&, Rectf) override { ++draws; }
    void draw_texture(const Texture&, Rectf, Rectf) override { ++draws; }
    void fill_rect(Rectf, Color) override {}
    void draw_rect(Rectf, Color) override {}
    void draw_line(Vec2f, Vec2f, Color) override {}
    void set_viewport(Rectf) override {}
    void reset_viewport() override {}
    void push_viewport(Rectf) override {}
    void pop_viewport() override {}
};

std::vector<std::string> labels(const std::vector<std::string>& words) {
    std::vector<std::string> out;
    for (int i = 0; i < 70; ++i) {
        out.push_back(words[static_cast<std::size_t>(i) % words.size()] + " " + std::to_string(i * 37));
    }
    return out;
}

f64 frame_us(const ui2::Font& font, const std::vector<std::string>& text) {
    Renderer2D renderer{std::make_unique<CountingBackend>()};
    constexpr int frames = 400;
    std::vector<f64> runs;
    for (int run = 0; run < 5; ++run) {
        const auto start = std::chrono::steady_clock::now();
        for (int frame = 0; frame < frames; ++frame) {
            f32 y = 0.0f;
            for (const std::string& label : text) {
                const Vec2f size = ui2::measure_text(font, label, 1.0f);
                ui2::draw_text(renderer, font, label, {1000.0f - size.x, y}, 1.0f, colors::white);
                y += size.y;
            }
        }
        const std::chrono::duration<f64, std::micro> spent = std::chrono::steady_clock::now() - start;
        runs.push_back(spent.count() / frames);
    }
    std::ranges::sort(runs);
    return runs[runs.size() / 2];
}

} // namespace

int main(int argc, char** argv) {
    const std::string only = argc > 1 ? argv[1] : "";
    if (!ui2::system_ui_font_available()) {
        std::printf("no system font\n");
        return 0;
    }
    const ui2::Font font = ui2::system_ui_font(16);
    const auto ascii = labels({"Market of Eldoria", "Iron ore", "Grain", "Caravan to Westmarch", "Silk"});
    const auto accented = labels({"March\xC3\xA9 d'\xC3\x89ldoria", "Minerai de fer", "Bl\xC3\xA9", "Gr\xC3\xBC\xC3\x9F" "e", "Soie"});
    const auto cyrillic = labels({"\xD0\xA0\xD1\x8B\xD0\xBD\xD0\xBE\xD0\xBA", "\xD0\x96\xD0\xB5\xD0\xBB\xD0\xB5\xD0\xB7\xD0\xBE",
                                  "\xD0\x97\xD0\xB5\xD1\x80\xD0\xBD\xD0\xBE"});
    // An argument runs one set alone (for a profiler).
    if (only.empty() || only == "ascii") std::printf("ascii     %8.1f us/frame (70 labels)\n", frame_us(font, ascii));
    if (only.empty() || only == "accented") std::printf("accented  %8.1f us/frame\n", frame_us(font, accented));
    if (only.empty() || only == "cyrillic") std::printf("cyrillic  %8.1f us/frame\n", frame_us(font, cyrillic));
    return 0;
}
