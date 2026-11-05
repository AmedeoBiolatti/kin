// Pictures the material import reads that kin's images (SDL_image) don't: TIFF (baseline: strips or tiles,
// uncompressed, LZW, PackBits or Deflate, the horizontal predictor; 8, 16 or 32-bit float samples; grey, grey and
// alpha, RGB, RGBA, palette; chunky or planar) and Photoshop's PSD (its composited picture, by stb_image).
#include "image_decode3d.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" {
unsigned char* kin_psd_load(const unsigned char* bytes, int size, int* width, int* height);
char* kin_zlib_inflate(const char* bytes, int size, int expected, int* out_size);
void kin_import_free(void* pixels);
}

namespace kin::material_import {
namespace {

class Tiff {
public:
    explicit Tiff(std::span<const u8> bytes) : _b(bytes) {
        if (_b.size() < 8)
            fail("too short");
        _little = _b[0] == 'I';
        if ((_b[0] != 'I' && _b[0] != 'M') || _b[1] != _b[0] || u16_at(2) != 42)
            fail("not a TIFF (or a BigTIFF)");
    }

    Image decode() {
        read_ifd(u32_at(4));
        const u32 width = tag(256), height = tag(257);
        const u32 spp = tag(277, 1), compression = tag(259, 1), photometric = tag(262, 2);
        const u32 planar = tag(284, 1), predictor = tag(317, 1), format = tag(339, 1);
        const u32 bits = tag(258, 1);
        if (!width || !height || width > 32768 || height > 32768)
            fail("an unusable size");
        if (bits != 8 && bits != 16 && !(bits == 32 && format == 3))
            fail(std::to_string(bits) + "-bit samples (format " + std::to_string(format) + ")");
        if (spp < 1 || spp > 16)
            fail("an unusable number of samples");
        if (photometric > 3)
            fail("photometric interpretation " + std::to_string(photometric) + " (CMYK, YCbCr, Lab...)");
        if (predictor != 1 && predictor != 2)
            fail("predictor " + std::to_string(predictor));
        const bool tiled = _tags.contains(322);
        const u32 piece_w = tiled ? tag(322) : width, piece_h = tiled ? tag(323) : std::min(tag(278, height), height);
        if (!piece_w || !piece_h)
            fail("empty strips or tiles");
        const auto offsets = values(tiled ? 324 : 273), counts = values(tiled ? 325 : 279);
        if (offsets.empty() || offsets.size() != counts.size())
            fail("no strips or tiles");
        const u32 across = (width + piece_w - 1) / piece_w, down = (height + piece_h - 1) / piece_h;
        const u32 planes = planar == 2 ? spp : 1, per_pixel = planar == 2 ? 1 : spp;
        if (offsets.size() < size_t(across) * down * planes)
            fail("too few strips or tiles");
        const size_t sample_bytes = bits / 8;
        // The picture's samples, each plane after the other (planar) or interleaved (chunky), as the file's bytes.
        std::vector<u8> samples(size_t(width) * height * spp * sample_bytes);
        const size_t piece_row = size_t(piece_w) * per_pixel * sample_bytes;
        for (u32 plane = 0; plane < planes; ++plane)
            for (u32 py = 0; py < down; ++py)
                for (u32 px = 0; px < across; ++px) {
                    const size_t index = (size_t(plane) * down + py) * across + px;
                    std::vector<u8> piece = inflate(offsets[index], counts[index], compression, piece_row * piece_h);
                    if (piece.size() < piece_row * std::min(piece_h, height - py * piece_h))
                        piece.resize(piece_row * piece_h, 0);
                    if (predictor == 2)
                        undo_predictor(piece, piece_w, piece_h, per_pixel, sample_bytes);
                    for (u32 y = 0; y < piece_h && py * piece_h + y < height; ++y)
                        for (u32 x = 0; x < piece_w && px * piece_w + x < width; ++x) {
                            const size_t to = ((size_t(py * piece_h + y) * width + px * piece_w + x) * spp +
                                               (planar == 2 ? plane : 0)) *
                                              sample_bytes;
                            const size_t from = (size_t(y) * piece_w + x) * per_pixel * sample_bytes;
                            if (from + per_pixel * sample_bytes <= piece.size())
                                std::memcpy(&samples[to], &piece[from], per_pixel * sample_bytes);
                        }
                }
        // As 8-bit RGBA.
        const auto sample = [&](size_t pixel, u32 s) -> f32 {
            const u8* p = &samples[(pixel * spp + s) * sample_bytes];
            if (bits == 8)
                return p[0] / 255.0f;
            if (bits == 16)
                return (_little ? u32(p[0]) | u32(p[1]) << 8 : u32(p[1]) | u32(p[0]) << 8) / 65535.0f;
            u32 v = _little ? u32(p[0]) | u32(p[1]) << 8 | u32(p[2]) << 16 | u32(p[3]) << 24
                            : u32(p[3]) | u32(p[2]) << 8 | u32(p[1]) << 16 | u32(p[0]) << 24;
            f32 f;
            std::memcpy(&f, &v, 4);
            return std::isfinite(f) ? f : 0.0f;
        };
        const auto palette = photometric == 3 ? values(320) : std::vector<u32>{};
        if (photometric == 3 && (bits != 8 || palette.size() < 3 * 256))
            fail("a palette other than 8-bit");
        const auto extra = values(338);
        const bool associated = !extra.empty() && extra[0] == 1 && (spp == 2 || spp == 4);
        Image image{Vec2i{int(width), int(height)}, std::vector<u8>(size_t(width) * height * 4)};
        const auto byte = [](f32 v) { return u8(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
        for (size_t i = 0; i < size_t(width) * height; ++i) {
            u8* o = &image.rgba[i * 4];
            f32 r, g, b, a = 1.0f;
            if (photometric == 3) {
                const u32 k = samples[i * spp];
                r = palette[k] / 65535.0f;
                g = palette[256 + k] / 65535.0f;
                b = palette[512 + k] / 65535.0f;
            } else if (spp >= 3 && photometric == 2) {
                r = sample(i, 0), g = sample(i, 1), b = sample(i, 2);
                if (spp >= 4)
                    a = sample(i, 3);
            } else {
                r = g = b = photometric == 0 ? 1.0f - sample(i, 0) : sample(i, 0);
                if (spp >= 2)
                    a = sample(i, 1);
            }
            if (associated && a > 0.0f) // premultiplied: as kin's maps are, straight
                r /= a, g /= a, b /= a;
            o[0] = byte(r), o[1] = byte(g), o[2] = byte(b), o[3] = byte(a);
        }
        return image;
    }

private:
    [[noreturn]] static void fail(const std::string& why) { throw std::runtime_error("TIFF: " + why); }
    u8 byte_at(size_t at) const {
        if (at >= _b.size())
            fail("truncated");
        return _b[at];
    }
    u16 u16_at(size_t at) const {
        if (at + 2 > _b.size())
            fail("truncated");
        return _little ? u16(_b[at] | _b[at + 1] << 8) : u16(_b[at] << 8 | _b[at + 1]);
    }
    u32 u32_at(size_t at) const {
        if (at + 4 > _b.size())
            fail("truncated");
        return _little ? u32(_b[at]) | u32(_b[at + 1]) << 8 | u32(_b[at + 2]) << 16 | u32(_b[at + 3]) << 24
                       : u32(_b[at + 3]) | u32(_b[at + 2]) << 8 | u32(_b[at + 1]) << 16 | u32(_b[at]) << 24;
    }
    struct Entry {
        u16 type;
        u32 count, at; // where its values are
    };
    void read_ifd(u32 at) {
        const u16 n = u16_at(at);
        for (u16 i = 0; i < n; ++i) {
            const size_t e = at + 2 + size_t(i) * 12;
            const u16 id = u16_at(e), type = u16_at(e + 2);
            const u32 count = u32_at(e + 4);
            const u32 size = type == 3 ? 2 : type == 4 || type == 9 ? 4 : 1;
            _tags[id] = {type, count, u32(size_t(size) * count <= 4 ? e + 8 : u32_at(e + 8))};
        }
    }
    std::vector<u32> values(u16 id) const {
        const auto it = _tags.find(id);
        if (it == _tags.end())
            return {};
        const Entry& t = it->second;
        if (t.count > (1u << 24))
            fail("a tag too long");
        std::vector<u32> out(t.count);
        for (u32 i = 0; i < t.count; ++i)
            out[i] = t.type == 3                  ? u16_at(t.at + 2 * i)
                     : t.type == 4 || t.type == 9 ? u32_at(t.at + 4 * i)
                                                  : byte_at(t.at + i);
        return out;
    }
    u32 tag(u16 id, u32 fallback = 0) const {
        const auto v = values(id);
        return v.empty() ? fallback : v[0];
    }

    std::vector<u8> inflate(u32 offset, u32 count, u32 compression, size_t expected) const {
        if (size_t(offset) + count > _b.size())
            fail("a strip past the end");
        const u8* in = _b.data() + offset;
        std::vector<u8> out;
        out.reserve(expected);
        if (compression == 1) {
            out.assign(in, in + count);
        } else if (compression == 32773) { // PackBits
            for (u32 i = 0; i < count;) {
                const int8_t n = int8_t(in[i++]);
                if (n >= 0) {
                    const u32 run = u32(n) + 1;
                    if (i + run > count)
                        break;
                    out.insert(out.end(), in + i, in + i + run);
                    i += run;
                } else if (n != -128 && i < count) {
                    out.insert(out.end(), size_t(1 - n), in[i++]);
                }
            }
        } else if (compression == 5) {
            lzw(in, count, out, expected);
        } else if (compression == 8 || compression == 32946) {
            int size = 0;
            char* inflated = kin_zlib_inflate(reinterpret_cast<const char*>(in), int(count), int(expected), &size);
            if (!inflated)
                fail("a Deflate strip that does not inflate");
            out.assign(inflated, inflated + size);
            kin_import_free(inflated);
        } else {
            fail("compression " + std::to_string(compression) + " (JPEG, CCITT...)");
        }
        return out;
    }

    // TIFF's LZW: codes of 9 to 12 bits, most significant first, 256 clears, 257 ends; the code width grows one code
    // early ("early change").
    static void lzw(const u8* in, u32 count, std::vector<u8>& out, size_t expected) {
        std::vector<std::pair<u32, u8>> table; // a code: (its prefix's code, its last byte)
        std::vector<u8> scratch;
        const auto reset = [&] {
            table.resize(258);
            for (u32 i = 0; i < 256; ++i)
                table[i] = {~0u, u8(i)};
        };
        const auto emit = [&](u32 code) {
            scratch.clear();
            for (u32 c = code; c != ~0u; c = table[c].first)
                scratch.push_back(table[c].second);
            out.insert(out.end(), scratch.rbegin(), scratch.rend());
            return scratch.back(); // its first byte
        };
        reset();
        u32 width = 9, previous = ~0u;
        u64 bits = 0;
        u32 held = 0;
        for (u32 i = 0;;) {
            while (held < width && i < count)
                bits = bits << 8 | in[i++], held += 8;
            if (held < width || out.size() >= expected + 4096)
                break;
            const u32 code = u32(bits >> (held - width)) & ((1u << width) - 1);
            held -= width;
            if (code == 257)
                break;
            if (code == 256) {
                reset();
                width = 9;
                previous = ~0u;
                continue;
            }
            if (previous == ~0u) {
                if (code >= 256)
                    break;
                emit(code);
            } else if (code < table.size()) {
                const u8 first = emit(code);
                table.push_back({previous, first});
            } else if (code == table.size()) {
                const u8 first = emit(previous);
                out.push_back(first);
                table.push_back({previous, first});
            } else {
                break; // a broken stream: what came so far
            }
            previous = code;
            if (table.size() + 1 >= (1u << width) && width < 12)
                ++width;
        }
    }

    void undo_predictor(std::vector<u8>& piece, u32 w, u32 h, u32 per_pixel, size_t sample_bytes) const {
        const size_t row = size_t(w) * per_pixel * sample_bytes;
        for (u32 y = 0; y < h && (y + 1) * row <= piece.size(); ++y) {
            u8* r = &piece[y * row];
            if (sample_bytes == 1) {
                for (size_t i = per_pixel; i < size_t(w) * per_pixel; ++i)
                    r[i] = u8(r[i] + r[i - per_pixel]);
            } else if (sample_bytes == 2) {
                const auto get = [&](size_t i) {
                    return _little ? u16(r[2 * i] | r[2 * i + 1] << 8) : u16(r[2 * i] << 8 | r[2 * i + 1]);
                };
                const auto put = [&](size_t i, u16 v) {
                    r[2 * i + (_little ? 0 : 1)] = u8(v);
                    r[2 * i + (_little ? 1 : 0)] = u8(v >> 8);
                };
                for (size_t i = per_pixel; i < size_t(w) * per_pixel; ++i)
                    put(i, u16(get(i) + get(i - per_pixel)));
            } else {
                fail("the horizontal predictor on 32-bit samples");
            }
        }
    }

    std::span<const u8> _b;
    bool _little = true;
    std::map<u16, Entry> _tags;
};

} // namespace

std::optional<Image> decode_extra_image(std::span<const u8> bytes, std::string* why) {
    try {
        if (bytes.size() >= 4 && ((bytes[0] == 'I' && bytes[1] == 'I') || (bytes[0] == 'M' && bytes[1] == 'M')))
            return Tiff(bytes).decode();
        if (bytes.size() >= 4 && std::memcmp(bytes.data(), "8BPS", 4) == 0) {
            int w = 0, h = 0;
            unsigned char* pixels = kin_psd_load(bytes.data(), int(bytes.size()), &w, &h);
            if (!pixels)
                throw std::runtime_error("PSD: not one stb_image reads (16-bit, or without its composited picture)");
            Image image{Vec2i{w, h}, std::vector<u8>(pixels, pixels + size_t(w) * h * 4)};
            kin_import_free(pixels);
            return image;
        }
        throw std::runtime_error("not a TIFF or PSD");
    } catch (const std::exception& e) {
        if (why)
            *why = e.what();
        return std::nullopt;
    }
}

} // namespace kin::material_import
