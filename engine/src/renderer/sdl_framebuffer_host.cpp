#include "compact_renderer2d_backend.hpp"
#include <kin/platform/window.hpp>
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <stdexcept>

namespace kin {
namespace {
[[noreturn]] void sdl_error(const char* operation) {
    throw std::runtime_error(std::string("compact: ") + operation + ": " + SDL_GetError());
}
class SdlFramebufferHost final : public FramebufferHost {
public:
    SdlFramebufferHost(SDL_Window* window, bool vsync) : _window(window) {
        SDL_SetHint(SDL_HINT_FRAMEBUFFER_ACCELERATION, "0");
        if (!SDL_GetWindowSurface(_window)) sdl_error("SDL_GetWindowSurface");
        SDL_SetWindowSurfaceVSync(_window, vsync ? 1 : 0);
    }
    Vec2i pixel_size() const override {
        Vec2i size;
        if (!SDL_GetWindowSizeInPixels(_window, &size.x, &size.y)) sdl_error("SDL_GetWindowSizeInPixels");
        return size;
    }
    Vec2i window_size() const override {
        Vec2i size;
        if (!SDL_GetWindowSize(_window, &size.x, &size.y)) sdl_error("SDL_GetWindowSize");
        return size;
    }
    void present_rgba(std::span<const u8> pixels, Vec2i size) override {
        auto* surface = SDL_GetWindowSurface(_window); // re-fetch after every resize
        if (!surface) sdl_error("SDL_GetWindowSurface");
        const auto* format = SDL_GetPixelFormatDetails(surface->format);
        if (!format || format->bytes_per_pixel < 2 || format->bytes_per_pixel > 4 || !format->Rmask || !format->Gmask || !format->Bmask)
            throw std::runtime_error("compact: unsupported window surface pixel format");
        const auto pack = [](u8 channel, u32 mask, u8 shift) {
            return mask ? u32((u64(channel) * (mask >> shift) + 127) / 255) << shift : 0;
        };
        // At -Os, general bit-depth scaling otherwise performs millions of integer
        // divisions per frame. Build the conversion once per native format instead.
        if (_pack_format != u32(surface->format)) {
            for (unsigned i = 0; i < 256; ++i) {
                _pack[0][i] = pack(u8(i), format->Rmask, format->Rshift);
                _pack[1][i] = pack(u8(i), format->Gmask, format->Gshift);
                _pack[2][i] = pack(u8(i), format->Bmask, format->Bshift);
                _pack[3][i] = pack(u8(i), format->Amask, format->Ashift);
            }
            _pack_format = u32(surface->format);
        }
        if (!SDL_LockSurface(surface)) sdl_error("SDL_LockSurface");
        const int bytes = format->bytes_per_pixel;
        for (int y = 0; y < std::min(size.y, surface->h); ++y) {
            auto* row = static_cast<u8*>(surface->pixels) + std::size_t(y) * surface->pitch;
            for (int x = 0; x < std::min(size.x, surface->w); ++x) {
                const u8* p = pixels.data() + (std::size_t(y) * size.x + x) * 4;
                const u32 value = _pack[0][p[0]] | _pack[1][p[1]] | _pack[2][p[2]] | _pack[3][p[3]];
                for (int byte = 0; byte < bytes; ++byte) {
#if SDL_BYTEORDER == SDL_LIL_ENDIAN
                    row[x * bytes + byte] = u8(value >> (byte * 8));
#else
                    row[x * bytes + byte] = u8(value >> ((bytes - byte - 1) * 8));
#endif
                }
            }
        }
        SDL_UnlockSurface(surface);
        if (!SDL_UpdateWindowSurface(_window)) sdl_error("SDL_UpdateWindowSurface");
    }

private:
    SDL_Window* _window;
    std::array<std::array<u32, 256>, 4> _pack;
    u32 _pack_format = 0;
};
}
CompactRenderer2DBackend::CompactRenderer2DBackend(Window& window, bool vsync)
    : _owned_host(std::make_unique<SdlFramebufferHost>(static_cast<SDL_Window*>(window.native_handle()), vsync)),
      _host(_owned_host.get()) {
    sync_size();
}
} // namespace kin
