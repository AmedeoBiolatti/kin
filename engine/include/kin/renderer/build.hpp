#pragma once

#include <cstdlib>
#include <stdexcept>
#include <string_view>

namespace kin {

enum class RendererRequest { Auto, Sdl, Gpu, Compact };

// Validate before SDL initialization, so an incompatible override reports the
// build policy even on machines without a display. No process-global cache.
inline RendererRequest renderer_request(const char* value) {
    const std::string_view choice = value ? value : "";
    if (choice != "" && choice != "sdl" && choice != "gpu" && choice != "compact") {
        throw std::invalid_argument("KIN_RENDER_BACKEND must be sdl, gpu or compact (or unset)");
    }
#if KIN_HAS_COMPACT_RENDERER
    if (choice != "" && choice != "compact") {
        throw std::invalid_argument("KIN_RENDER_BACKEND conflicts with KIN_RENDERER=compact");
    }
    return RendererRequest::Compact;
#else
    if (choice == "compact") {
        throw std::invalid_argument("KIN_RENDER_BACKEND=compact requires KIN_RENDERER=compact");
    }
#if !KIN_HAS_GPU_RENDERER
    if (choice == "gpu") {
        throw std::invalid_argument("KIN_RENDER_BACKEND=gpu conflicts with KIN_RENDERER=software");
    }
    return RendererRequest::Sdl;
#elif !KIN_HAS_SDL_RENDERER
    if (choice == "sdl") {
        throw std::invalid_argument("KIN_RENDER_BACKEND=sdl conflicts with KIN_RENDERER=gpu");
    }
    return RendererRequest::Gpu;
#else
    return choice == "gpu" ? RendererRequest::Gpu :
           choice == "sdl" ? RendererRequest::Sdl : RendererRequest::Auto;
#endif
#endif
}

inline RendererRequest requested_renderer() {
    return renderer_request(std::getenv("KIN_RENDER_BACKEND"));
}

} // namespace kin
