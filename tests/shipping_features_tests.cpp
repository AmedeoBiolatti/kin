#include <kin/platform/app.hpp>
#include <kin/renderer/build.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <kin/renderer/shader_compiler.hpp>
#include <kin/ui2/baked_font.hpp>

#include <cassert>
#include <stdexcept>
#include <vector>

int main() {
    bool refused = false;
    try { kin::renderer_request("typo"); }
    catch (const std::invalid_argument&) { refused = true; }
    assert(refused);
#if !KIN_HAS_GPU_RENDERER
    assert(kin::renderer_request(nullptr) == kin::RendererRequest::Sdl);
    assert(kin::renderer_request("sdl") == kin::RendererRequest::Sdl);
    refused = false;
    try { kin::renderer_request("gpu"); }
    catch (const std::invalid_argument&) { refused = true; }
    assert(refused);
    assert(!kin::shader_compiler_available());
#elif !KIN_HAS_SDL_RENDERER
    assert(kin::renderer_request(nullptr) == kin::RendererRequest::Gpu);
    refused = false;
    try { kin::renderer_request("sdl"); }
    catch (const std::invalid_argument&) { refused = true; }
    assert(refused);
#else
    assert(kin::renderer_request(nullptr) == kin::RendererRequest::Auto);
#endif
    kin::App app{{.mode = kin::AppMode::Headless}};
    auto& window = app.create_window({.title = "shipping features", .width = 32, .height = 32, .hidden = true});
    kin::Renderer2D renderer{kin::make_render_backend(window, false, false)};
#if !KIN_HAS_GPU_RENDERER
    assert(renderer.backend_name() == "software");
    assert(!renderer.capabilities().materials_2d);
    assert(!renderer.create_shader({}));
    assert(!renderer.create_compute_shader({}));
    assert(!renderer.builtin_shader(kin::BuiltinShader::Vignette));
#elif !KIN_HAS_SDL_RENDERER
    assert(renderer.backend_name() == "SDL_GPU");
    assert(renderer.capabilities().materials_2d);
#endif
#if !KIN_ENABLE_IMAGE_IO
    assert(!renderer.save_png("unused.png"));
#endif
#if !KIN_ENABLE_TTF
    assert(!kin::ui2::system_ui_font_available());
    refused = false;
    try { kin::ui2::load_ttf_font("unused.ttf", 15); }
    catch (const std::runtime_error&) { refused = true; }
    assert(refused);
#endif
    // Baked coverage, kerning, newlines, fallback, and GPU/software upload.
    constexpr kin::u8 coverage[]{1, 255, 1, 0};
    constexpr kin::ui2::BakedGlyph glyphs[]{
        {'?', {0, 0, 1, 1}, {}, 2}, {'A', {0, 0, 1, 1}, {}, 2}, {'V', {0, 0, 1, 1}, {}, 2}};
    constexpr kin::ui2::BakedKerning kerning[]{{'A', 'V', -1}};
    auto font = kin::ui2::baked_font({{2, 1}, 3, coverage, glyphs, kerning});
    assert((kin::ui2::measure_text(font, "AV") == kin::Vec2f{3, 3}));
    assert((kin::ui2::measure_text(font, "A\nV") == kin::Vec2f{2, 6}));
    assert((kin::ui2::measure_text(font, "!") == kin::Vec2f{2, 3}));
    renderer.clear(kin::colors::black);
    kin::ui2::draw_text(renderer, font, "AV", {2, 2}, 2, kin::colors::white);
    std::vector<kin::u8> rgba;
    kin::Vec2i size;
    assert(renderer.read_rgba({0, 0, 32, 32}, rgba, size));
    assert(rgba[(2 * 32 + 2) * 4] >= 128);
    assert(rgba[(8 * 32 + 8) * 4] == 0);
    refused = false;
    try { kin::ui2::baked_font({{3, 1}, 3, coverage, glyphs, kerning}); }
    catch (const std::invalid_argument&) { refused = true; }
    assert(refused);
}
