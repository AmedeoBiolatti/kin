// Pictures the material import reads that kin's images (SDL_image) don't: TIFF (baseline: strips or tiles,
// uncompressed, LZW, PackBits or Deflate, the horizontal predictor; 8, 16 or 32-bit float samples; grey, grey and
// alpha, RGB, RGBA, palette; chunky or planar) and Photoshop's PSD (its composited picture, by stb_image).
#include "image_decode3d.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
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

namespace {
f32 half_to_float(u16 h) {
    const u32 sign = u32(h >> 15) << 31, exponent = (h >> 10) & 31, mantissa = h & 1023;
    u32 bits = 0;
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else { // subnormal
            f32 v = std::ldexp(f32(mantissa), -24);
            return (h >> 15) ? -v : v;
        }
    } else if (exponent == 31) {
        bits = sign | 0x7f800000u | (mantissa << 13);
    } else {
        bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
    }
    f32 out;
    std::memcpy(&out, &bits, 4);
    return out;
}

// Float channels (`which`: each of R, G, B, A's plane, -1: none, then 0, alpha 1) as 8-bit RGBA, each channel fitted
// into 0 to 1 over its least and largest value where they leave it (said in `range`).
// With `log`, R, G and B (light: none below 0) by their logarithm, over the least to the largest (the least at most 16
// stops below: darker reads as that).
Image fit(const std::vector<std::vector<f32>>& planes, const std::array<int, 4>& which, size_t width, size_t height,
          ImageRange* range, bool log = false) {
    ImageRange fitted;
    fitted.log = log;
    const auto logarithmic = [&](int k) { return log && k < 3; };
    for (int k = 0; k < 4; ++k) {
        if (which[k] < 0)
            continue;
        const auto [lo, hi] = std::minmax_element(planes[size_t(which[k])].begin(), planes[size_t(which[k])].end());
        if (logarithmic(k)) {
            const f32 top = *hi > 0.0f ? *hi : 1.0f, bottom = std::max(*lo, top / 65536.0f);
            fitted.bias[k] = std::log2(bottom);
            fitted.scale[k] = top > bottom ? std::log2(top) - fitted.bias[k] : 1.0f;
        } else if (*lo < 0.0f || *hi > 1.0f) {
            fitted.bias[k] = *lo;
            fitted.scale[k] = *hi > *lo ? *hi - *lo : 1.0f;
        }
    }
    Image image{Vec2i{i32(width), i32(height)}, std::vector<u8>(width * height * 4)};
    for (size_t i = 0; i < width * height; ++i)
        for (int k = 0; k < 4; ++k) {
            f32 v = which[k] < 0 ? (k == 3 ? 1.0f : 0.0f) : planes[size_t(which[k])][i];
            if (which[k] >= 0)
                v = ((logarithmic(k) ? std::log2(std::max(v, std::exp2(fitted.bias[k]))) : v) - fitted.bias[k]) /
                    fitted.scale[k];
            image.rgba[i * 4 + size_t(k)] = u8(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
    if (range)
        *range = fitted;
    return image;
}

// Radiance RGBE (.hdr, Unreal's export of a cube map: unwrapped to longitude and latitude): its header, a "-Y h +X w"
// size, then scanlines flat, in the old run-length coding or the new (each component's run-length coded apart). Its
// colour fitted logarithmically (a sky's precision beside its sun's).
Image decode_hdr(std::span<const u8> b, ImageRange* range) {
    const auto fail = [](const std::string& why) { throw std::runtime_error("Radiance HDR: " + why); };
    size_t at = 0;
    const auto line = [&] {
        std::string out;
        while (at < b.size() && b[at] != '\n')
            out += char(b[at++]);
        if (at >= b.size())
            fail("cut short in its header");
        ++at;
        return out;
    };
    bool rgbe = false;
    for (std::string l = line(); !l.empty(); l = line())
        if (l.starts_with("FORMAT=")) {
            if (l != "FORMAT=32-bit_rle_rgbe")
                fail(l + " (not RGBE)");
            rgbe = true;
        }
    if (!rgbe)
        fail("no FORMAT=32-bit_rle_rgbe");
    int width = 0, height = 0;
    if (std::sscanf(line().c_str(), "-Y %d +X %d", &height, &width) != 2 || width <= 0 || height <= 0 ||
        width > 32768 || height > 32768)
        fail("not a -Y h +X w picture");
    const size_t w = size_t(width), h = size_t(height);
    std::vector<u8> texels(w * h * 4);
    const auto byte = [&] {
        if (at >= b.size())
            fail("cut short");
        return b[at++];
    };
    for (size_t y = 0; y < h; ++y) {
        u8* row = texels.data() + y * w * 4;
        if (w >= 8 && w < 32768 && at + 4 <= b.size() && b[at] == 2 && b[at + 1] == 2 && (b[at + 2] & 0x80) == 0) {
            if ((size_t(b[at + 2]) << 8 | b[at + 3]) != w)
                fail("a scanline of the wrong width");
            at += 4;
            for (size_t c = 0; c < 4; ++c)
                for (size_t x = 0; x < w;) {
                    size_t count = byte();
                    const bool run = count > 128;
                    if (run)
                        count -= 128;
                    if (count == 0 || x + count > w)
                        fail("a run past its scanline");
                    const u8 v = run ? byte() : 0;
                    for (size_t k = 0; k < count; ++k)
                        row[(x++) * 4 + c] = run ? v : byte();
                }
            continue;
        }
        for (size_t x = 0, shift = 0; x < w;) { // flat, or the old coding (1, 1, 1, n: the last texel n << shift times)
            u8 t[4] = {byte(), byte(), byte(), byte()};
            if (t[0] == 1 && t[1] == 1 && t[2] == 1) {
                if (x == 0)
                    fail("a repeat with nothing before it");
                const size_t count = size_t(t[3]) << shift;
                if (x + count > w)
                    fail("a run past its scanline");
                for (size_t k = 0; k < count; ++k, ++x)
                    std::memcpy(row + x * 4, row + (x - 1) * 4, 4);
                shift += 8;
                continue;
            }
            std::memcpy(row + x * 4, t, 4);
            ++x;
            shift = 0;
        }
    }
    std::vector<std::vector<f32>> planes(3, std::vector<f32>(w * h));
    for (size_t i = 0; i < w * h; ++i) {
        const u8* t = texels.data() + i * 4;
        const f32 f = t[3] ? std::ldexp(1.0f, int(t[3]) - (128 + 8)) : 0.0f;
        for (size_t c = 0; c < 3; ++c)
            planes[c][i] = t[3] ? (f32(t[c]) + 0.5f) * f : 0.0f;
    }
    return fit(planes, {0, 1, 2, -1}, w, h, range, true);
}

// OpenEXR, scanline images: no compression, RLE-free ZIPS or ZIP (zlib, a byte predictor and the bytes split in two
// halves); HALF or FLOAT channels R, G, B, A (or Y: grey). Each channel fitted into 0 to 1 over its least and largest
// value where they leave it (said in `range`).
Image decode_exr(std::span<const u8> b, ImageRange* range) {
    const auto fail = [](const std::string& why) { throw std::runtime_error("OpenEXR: " + why); };
    const auto u32_at = [&](size_t at) {
        if (at + 4 > b.size())
            fail("cut short");
        u32 v;
        std::memcpy(&v, b.data() + at, 4);
        return v;
    };
    if (b.size() < 8 || u32_at(0) != 20000630u)
        fail("not an OpenEXR file");
    if (u32_at(4) & 0x200u)
        fail("tiled images");
    size_t at = 8;
    struct Channel {
        std::string name;
        u32 type;
    };
    std::vector<Channel> channels;
    i32 window[4] = {0, 0, -1, -1};
    int compression = -1;
    const auto text = [&]() {
        const auto* start = b.data() + at;
        const auto* end = static_cast<const u8*>(std::memchr(start, 0, b.size() - at));
        if (!end)
            fail("cut short");
        std::string s(reinterpret_cast<const char*>(start), size_t(end - start));
        at += s.size() + 1;
        return s;
    };
    while (true) {
        const std::string name = text();
        if (name.empty())
            break;
        const std::string type = text();
        const u32 size = u32_at(at);
        at += 4;
        if (at + size > b.size())
            fail("cut short");
        const u8* value = b.data() + at;
        if (name == "channels" && type == "chlist") {
            size_t p = 0;
            while (p < size && value[p]) {
                const auto* end = static_cast<const u8*>(std::memchr(value + p, 0, size - p));
                if (!end || size_t(end - value) + 17 > size)
                    fail("a broken channel list");
                std::string channel(reinterpret_cast<const char*>(value + p), size_t(end - (value + p)));
                u32 t;
                std::memcpy(&t, end + 1, 4);
                channels.push_back({channel, t});
                p = size_t(end - value) + 17;
            }
        } else if (name == "dataWindow" && type == "box2i" && size == 16) {
            std::memcpy(window, value, 16);
        } else if (name == "compression" && size == 1) {
            compression = value[0];
        }
        at += size;
    }
    const i64 width = i64(window[2]) - window[0] + 1, height = i64(window[3]) - window[1] + 1;
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384)
        fail("an unusable size");
    if (compression != 0 && compression != 2 && compression != 3)
        fail("compression " + std::to_string(compression) + " (none, ZIPS and ZIP are read)");
    for (const auto& c : channels)
        if (c.type != 1 && c.type != 2)
            fail("a channel of unsigned integers (" + c.name + ")");
    if (channels.empty())
        fail("no channels");
    // (Channels are stored in the order of their names: A, B, G, R.)
    const int lines = compression == 3 ? 16 : 1;
    const i64 blocks = (height + lines - 1) / lines;
    std::vector<std::vector<f32>> planes(channels.size(), std::vector<f32>(size_t(width * height)));
    for (i64 k = 0; k < blocks; ++k) {
        u64 offset;
        if (at + 8 * size_t(k) + 8 > b.size())
            fail("cut short");
        std::memcpy(&offset, b.data() + at + 8 * size_t(k), 8);
        if (offset + 8 > b.size())
            fail("a block past the end");
        const i32 y = i32(u32_at(size_t(offset))) - window[1];
        const u32 bytes = u32_at(size_t(offset) + 4);
        if (offset + 8 + bytes > b.size() || y < 0 || y >= height)
            fail("a broken block");
        const i64 rows = std::min<i64>(lines, height - y);
        size_t expected = 0;
        for (const auto& c : channels)
            expected += size_t(width) * size_t(rows) * (c.type == 1 ? 2 : 4);
        std::vector<u8> raw;
        const u8* data = b.data() + offset + 8;
        if (compression != 0 && bytes < expected) {
            int size = 0;
            char* out = kin_zlib_inflate(reinterpret_cast<const char*>(data), int(bytes), int(expected), &size);
            if (!out || size_t(size) != expected) {
                kin_import_free(out);
                fail("a block that does not inflate");
            }
            std::vector<u8> t(out, out + size);
            kin_import_free(out);
            for (size_t i = 1; i < t.size(); ++i) // the predictor undone
                t[i] = u8(t[i - 1] + t[i] - 128);
            raw.resize(t.size());
            const size_t half = (t.size() + 1) / 2;
            for (size_t i = 0; i < t.size(); ++i) // the two halves interleaved again
                raw[i] = (i % 2 == 0) ? t[i / 2] : t[half + i / 2];
            data = raw.data();
        } else if (bytes != expected) {
            fail("a block of the wrong size");
        }
        size_t p = 0;
        for (i64 r = 0; r < rows; ++r)
            for (size_t c = 0; c < channels.size(); ++c)
                for (i64 x = 0; x < width; ++x) {
                    f32 v;
                    if (channels[c].type == 1) {
                        u16 h;
                        std::memcpy(&h, data + p, 2);
                        v = half_to_float(h);
                        p += 2;
                    } else {
                        std::memcpy(&v, data + p, 4);
                        p += 4;
                    }
                    planes[c][size_t((y + r) * width + x)] = std::isfinite(v) ? v : 0.0f;
                }
    }
    // R, G, B, A from the channels by name (Y: grey; none: 0, alpha 1).
    std::array<int, 4> which{-1, -1, -1, -1};
    for (size_t c = 0; c < channels.size(); ++c) {
        const std::string& n = channels[c].name;
        if (n == "R" || n == "Y")
            which[0] = int(c);
        if (n == "G" || n == "Y")
            which[1] = int(c);
        if (n == "B" || n == "Y")
            which[2] = int(c);
        if (n == "A")
            which[3] = int(c);
    }
    return fit(planes, which, size_t(width), size_t(height), range);
}
} // namespace

std::optional<Image> decode_extra_image(std::span<const u8> bytes, std::string* why, ImageRange* range) {
    try {
        if (range)
            *range = {};
        if (bytes.size() >= 4 && bytes[0] == 0x76 && bytes[1] == 0x2f && bytes[2] == 0x31 && bytes[3] == 0x01)
            return decode_exr(bytes, range);
        if (bytes.size() >= 2 && bytes[0] == '#' && bytes[1] == '?')
            return decode_hdr(bytes, range);
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
        throw std::runtime_error("not a TIFF, PSD, OpenEXR or Radiance HDR");
    } catch (const std::exception& e) {
        if (why)
            *why = e.what();
        return std::nullopt;
    }
}

} // namespace kin::material_import
