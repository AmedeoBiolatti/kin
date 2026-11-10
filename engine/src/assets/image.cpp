#include <kin/assets/image.hpp>

#include <kin/platform/log.hpp>

#include <SDL3/SDL.h>
#if KIN_ENABLE_IMAGE_IO
#include <SDL3_image/SDL_image.h>
#endif

#include "content_io.hpp"

#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace kin {
namespace {

std::string path_string(const std::filesystem::path& path) {
    return path.string();
}

// The surface's pixels as tightly packed RGBA; frees it.
Image to_image(SDL_Surface* converted) {
    Image image;
    image.size = {converted->w, converted->h};
    image.rgba.resize(static_cast<std::size_t>(image.size.x * image.size.y * 4));
    const auto* src = static_cast<const u8*>(converted->pixels);
    const std::size_t row_bytes = static_cast<std::size_t>(image.size.x * 4);
    for (i32 row = 0; row < image.size.y; ++row) {
        std::memcpy(image.rgba.data() + static_cast<std::size_t>(row) * row_bytes,
                    src + static_cast<std::size_t>(row) * static_cast<std::size_t>(converted->pitch), row_bytes);
    }
    SDL_DestroySurface(converted);
    return image;
}

#if KIN_ENABLE_IMAGE_IO
u32 be32(const u8* p) {
    return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | u32(p[3]);
}

void put_be32(std::vector<u8>& out, u32 v) {
    for (int shift = 24; shift >= 0; shift -= 8)
        out.push_back(u8(v >> shift));
}

u32 png_crc(const u8* data, std::size_t size) {
    u32 crc = 0xffffffffu;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int k = 0; k < 8; ++k)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

// SDL_image 3.4's libpng loader (and its main branch, 2026-11) makes an 8-bit surface for a 16-bit greyscale PNG
// without asking libpng to strip the 16 bits: libpng writes two bytes a pixel into a one-byte-a-pixel surface, past
// its end by a whole picture (a 4096-square roughness map: 16 MB of heap). Such a PNG is given to it instead as 8-bit
// grey and alpha: the same two bytes a pixel, so the same scanlines, filters and compressed data, with each pixel's
// high byte read as its grey and its low byte as an alpha that alpha_from_low_byte() then sets opaque. Only IHDR,
// IDAT and IEND are kept: the other chunks (sBIT, tRNS, gamma, ...) describe the 16-bit picture. Empty: not such a
// PNG (or not a PNG), to be decoded as it is.
std::vector<u8> grey16_png_as_grey_alpha(std::span<const u8> bytes) {
    static constexpr u8 signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (bytes.size() < 8 + 25 || std::memcmp(bytes.data(), signature, 8) != 0 ||
        std::memcmp(bytes.data() + 12, "IHDR", 4) != 0 || be32(bytes.data() + 8) != 13)
        return {};
    const u8* ihdr = bytes.data() + 16;
    if (ihdr[8] != 16 || ihdr[9] != 0) // bit depth 16, colour type 0 (grey)
        return {};
    std::vector<u8> out(bytes.begin(), bytes.begin() + 8);
    const auto chunk = [&](const char* type, const u8* data, u32 size) {
        put_be32(out, size);
        const std::size_t start = out.size();
        out.insert(out.end(), type, type + 4);
        out.insert(out.end(), data, data + size);
        put_be32(out, png_crc(out.data() + start, out.size() - start));
    };
    u8 header[13];
    std::memcpy(header, ihdr, 13);
    header[8] = 8; // bit depth
    header[9] = 4; // grey and alpha
    chunk("IHDR", header, 13);
    std::size_t at = 8;
    bool ended = false;
    while (at + 12 <= bytes.size()) {
        const u32 size = be32(bytes.data() + at);
        if (size > bytes.size() - at - 12)
            return {}; // cut short: left to libpng to refuse
        const u8* type = bytes.data() + at + 4;
        if (std::memcmp(type, "IDAT", 4) == 0)
            chunk("IDAT", type + 4, size);
        else if (std::memcmp(type, "IEND", 4) == 0) {
            chunk("IEND", type + 4, 0);
            ended = true;
            break;
        }
        at += 12 + std::size_t(size);
    }
    return ended ? out : std::vector<u8>{};
}

void alpha_from_low_byte(Image& image) {
    for (std::size_t i = 3; i < image.rgba.size(); i += 4)
        image.rgba[i] = 255;
}

// The stream's picture (closing it), as SDL_image decodes it, with the 16-bit grey PNG guard above. `grey16` says
// whether the guard was used (the alpha to be set opaque after the conversion to RGBA).
SDL_Surface* load_surface(SDL_IOStream* stream, bool& grey16) {
    grey16 = false;
    if (!stream)
        return nullptr;
    std::size_t size = 0;
    void* data = SDL_LoadFile_IO(stream, &size, true);
    if (!data)
        return nullptr;
    const std::vector<u8> rewritten = grey16_png_as_grey_alpha({static_cast<const u8*>(data), size});
    grey16 = !rewritten.empty();
    SDL_IOStream* memory =
        grey16 ? SDL_IOFromConstMem(rewritten.data(), rewritten.size()) : SDL_IOFromConstMem(data, size);
    SDL_Surface* loaded = memory ? IMG_Load_IO(memory, true) : nullptr;
    SDL_free(data);
    return loaded;
}
#endif

} // namespace

Image decode_image(std::span<const u8> bytes, std::string_view name) {
#if KIN_ENABLE_IMAGE_IO
    bool grey16 = false;
    SDL_Surface* loaded = load_surface(SDL_IOFromConstMem(bytes.data(), bytes.size()), grey16);
    SDL_Surface* converted = loaded ? SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_RGBA32) : nullptr;
    if (loaded) {
        SDL_DestroySurface(loaded);
    }
    if (!converted) {
        KIN_LOG_ERROR_F("asset", "image decode failed",
                        (LogFields{{.name = "name", .value = std::string{name}}, {.name = "error", .value = SDL_GetError()}}));
        return {};
    }
    Image image = to_image(converted);
    if (grey16)
        alpha_from_low_byte(image);
    return image;
#else
    throw std::runtime_error("Image decoding requires KIN_ENABLE_IMAGE_IO");
#endif
}

Image rasterize_svg(std::string_view svg, Vec2i size, std::string_view name) {
#if KIN_ENABLE_IMAGE_IO
    if (size.x <= 0 || size.y <= 0) {
        return {};
    }
    // Text without an <svg> element is refused here, as IMG_isSVG does for
    // files: SDL_image's parse of one comes out empty, and it does not free it.
    if (svg.find("<svg") == std::string_view::npos) {
        KIN_LOG_ERROR_F("asset", "svg rasterize failed",
                        (LogFields{{.name = "name", .value = std::string{name}}, {.name = "error", .value = "no <svg> element"}}));
        return {};
    }
    SDL_IOStream* stream = SDL_IOFromConstMem(svg.data(), svg.size());
    SDL_Surface* loaded = stream ? IMG_LoadSizedSVG_IO(stream, size.x, size.y) : nullptr;
    if (stream) {
        SDL_CloseIO(stream);
    }
    SDL_Surface* converted = loaded ? SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_RGBA32) : nullptr;
    if (loaded) {
        SDL_DestroySurface(loaded);
    }
    if (!converted) {
        KIN_LOG_ERROR_F("asset", "svg rasterize failed",
                        (LogFields{{.name = "name", .value = std::string{name}}, {.name = "error", .value = SDL_GetError()}}));
        return {};
    }
    return to_image(converted);
#else
    static_cast<void>(svg);
    static_cast<void>(size);
    static_cast<void>(name);
    throw std::runtime_error("SVG rasterizing requires KIN_ENABLE_IMAGE_IO");
#endif
}

Image load_image(const std::filesystem::path& path) {
#if KIN_ENABLE_IMAGE_IO
    const std::string filename = path_string(path);
    bool grey16 = false;
    SDL_Surface* loaded = load_surface(open_content_stream(path), grey16);
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

    Image image = to_image(converted);
    if (grey16)
        alpha_from_low_byte(image);
    KIN_LOG_INFO_F("asset",
                   "image loaded",
                   (LogFields{
                       {.name = "path", .value = filename},
                       {.name = "type", .value = "Image"},
                       {.name = "width", .value = std::to_string(image.size.x)},
                       {.name = "height", .value = std::to_string(image.size.y)},
                   }));
    return image;
#else
    throw std::runtime_error("Image loading requires KIN_ENABLE_IMAGE_IO");
#endif
}

bool save_image(const Image& image, const std::filesystem::path& path) {
#if KIN_ENABLE_IMAGE_IO
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
#else
    return false;
#endif
}

} // namespace kin
