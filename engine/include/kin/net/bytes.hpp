#pragma once

// Little-endian packing for kin/net messages (docs/networking.md). A ByteReader never reads past its bytes: a short
// or damaged message makes ok() false and every later read return zero, so a parser reads everything and checks
// ok() once at the end.

#include <kin/core/types.hpp>

#include <bit>
#include <cstring>
#include <span>
#include <vector>

namespace kin::net {

class ByteWriter {
public:
    explicit ByteWriter(std::vector<u8>& out) : _out(out) {}

    void write_u8(u8 value) { _out.push_back(value); }
    void write_u16(u16 value) { write_le(value, 2); }
    void write_u32(u32 value) { write_le(value, 4); }
    void write_u64(u64 value) { write_le(value, 8); }
    void write_f32(f32 value) { write_u32(std::bit_cast<u32>(value)); }
    // LEB128: 7 bits a byte, low bits first; small values take one byte.
    void write_varint(u64 value) {
        while (value >= 0x80) {
            _out.push_back(static_cast<u8>(value | 0x80));
            value >>= 7;
        }
        _out.push_back(static_cast<u8>(value));
    }
    void write_bytes(std::span<const u8> bytes) { _out.insert(_out.end(), bytes.begin(), bytes.end()); }

    std::size_t size() const { return _out.size(); }

private:
    void write_le(u64 value, int bytes) {
        for (int i = 0; i < bytes; ++i) {
            _out.push_back(static_cast<u8>(value >> (8 * i)));
        }
    }

    std::vector<u8>& _out;
};

class ByteReader {
public:
    explicit ByteReader(std::span<const u8> bytes) : _bytes(bytes) {}

    u8 read_u8() { return static_cast<u8>(read_le(1)); }
    u16 read_u16() { return static_cast<u16>(read_le(2)); }
    u32 read_u32() { return static_cast<u32>(read_le(4)); }
    u64 read_u64() { return read_le(8); }
    f32 read_f32() { return std::bit_cast<f32>(read_u32()); }
    u64 read_varint() {
        u64 value = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            const u8 byte = read_u8();
            if (!_ok) {
                return 0;
            }
            // The tenth byte holds bit 63 alone; anything above it does not fit in 64 bits.
            if (shift == 63 && byte > 1) {
                break;
            }
            value |= static_cast<u64>(byte & 0x7f) << shift;
            if ((byte & 0x80) == 0) {
                return value;
            }
        }
        fail();
        return 0;
    }
    // The next `count` bytes, or an empty span (and ok() false) if fewer are left.
    std::span<const u8> read_bytes(std::size_t count) {
        if (!_ok || count > remaining()) {
            fail();
            return {};
        }
        const auto bytes = _bytes.subspan(_offset, count);
        _offset += count;
        return bytes;
    }

    bool ok() const { return _ok; }
    std::size_t remaining() const { return _ok ? _bytes.size() - _offset : 0; }
    bool at_end() const { return remaining() == 0; }

private:
    u64 read_le(int bytes) {
        if (!_ok || static_cast<std::size_t>(bytes) > remaining()) {
            fail();
            return 0;
        }
        u64 value = 0;
        for (int i = 0; i < bytes; ++i) {
            value |= static_cast<u64>(_bytes[_offset + static_cast<std::size_t>(i)]) << (8 * i);
        }
        _offset += static_cast<std::size_t>(bytes);
        return value;
    }
    void fail() {
        _ok = false;
        _offset = _bytes.size();
    }

    std::span<const u8> _bytes;
    std::size_t _offset = 0;
    bool _ok = true;
};

} // namespace kin::net
