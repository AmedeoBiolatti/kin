#include <kin/assets/image.hpp>

#include <kin/platform/log.hpp>

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>

#include <cstring>
#include <stdexcept>
#include <string>

#include "content_io.hpp"

namespace kin {
namespace {

std::string path_string(const std::filesystem::path& path) {
    return path.string();
}

} // namespace

Image load_image(const std::filesystem::path& path) {
    const std::string filename = path_string(path);
    SDL_IOStream* stream = open_content_stream(path);
    SDL_Surface* loaded = stream ? IMG_Load_IO(stream, true) : nullptr;
    if (!loaded) {
        const std::string error = "IMG_Load failed for " + filename + ": " + SDL_GetError();
        KIN_LOG_ERROR_F("asset",
                        "image load failed",
                        (LogFields{
                            {.name = "path", .value = filename},
                            {.name = "type", .value = "Image"},
                            {.name = "error", .value = error},
                        }));
        throw std::runtime_error(error);
    }

    SDL_Surface* converted = SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(loaded);
    if (!converted) {
        const std::string error = "SDL_ConvertSurface failed for " + filename + ": " + SDL_GetError();
        KIN_LOG_ERROR_F("asset",
                        "image conversion failed",
                        (LogFields{
                            {.name = "path", .value = filename},
                            {.name = "type", .value = "Image"},
                            {.name = "error", .value = error},
                        }));
        throw std::runtime_error(error);
    }

    Image image;
    image.size = {converted->w, converted->h};
    image.rgba.resize(static_cast<std::size_t>(image.size.x * image.size.y * 4));

    const auto* src = static_cast<const u8*>(converted->pixels);
    const std::size_t row_bytes = static_cast<std::size_t>(image.size.x * 4);
    for (i32 row = 0; row < image.size.y; ++row) {
        std::memcpy(
            image.rgba.data() + static_cast<std::size_t>(row) * row_bytes,
            src + static_cast<std::size_t>(row) * static_cast<std::size_t>(converted->pitch),
            row_bytes
        );
    }

    SDL_DestroySurface(converted);
    KIN_LOG_INFO_F("asset",
                   "image loaded",
                   (LogFields{
                       {.name = "path", .value = filename},
                       {.name = "type", .value = "Image"},
                       {.name = "width", .value = std::to_string(image.size.x)},
                       {.name = "height", .value = std::to_string(image.size.y)},
                   }));
    return image;
}

bool save_image(const Image& image, const std::filesystem::path& path) {
    if (!image.valid()) {
        KIN_LOG_ERROR_F("asset", "image save failed",
                        (LogFields{{.name = "path", .value = path_string(path)},
                                   {.name = "error", .value = "invalid image"}}));
        return false;
    }
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    // SDL_CreateSurfaceFrom wraps the caller's pixels (no copy); IMG_SavePNG reads them.
    SDL_Surface* surface = SDL_CreateSurfaceFrom(
        image.size.x, image.size.y, SDL_PIXELFORMAT_RGBA32,
        const_cast<u8*>(image.rgba.data()), image.size.x * 4);
    if (!surface) {
        KIN_LOG_ERROR_F("asset", "image save failed",
                        (LogFields{{.name = "path", .value = path_string(path)},
                                   {.name = "error", .value = SDL_GetError()}}));
        return false;
    }
    const bool ok = IMG_SavePNG(surface, path_string(path).c_str());
    SDL_DestroySurface(surface);
    if (!ok) {
        KIN_LOG_ERROR_F("asset", "image save failed",
                        (LogFields{{.name = "path", .value = path_string(path)},
                                   {.name = "error", .value = SDL_GetError()}}));
        return false;
    }
    KIN_LOG_INFO_F("asset", "image saved",
                   (LogFields{{.name = "path", .value = path_string(path)},
                              {.name = "width", .value = std::to_string(image.size.x)},
                              {.name = "height", .value = std::to_string(image.size.y)}}));
    return true;
}

} // namespace kin
