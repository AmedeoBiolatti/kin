#pragma once

// A folder of baked results: what a slow build made (a mesh's levels of detail or cluster hierarchy, a cooked
// collision shape), kept as bytes under a key, so that the next launch reads them back instead of building again.
//
//     kin::BakeCache bake{kin::user_data_dir("my-game") / "kin" / "bake"}; // the game picks the folder
//     r3d.set_bake_cache(&bake);                                          // levels and clusters (kin/render3d)
//     collisions.set_bake_cache(&bake);                                   // cooked shapes (kin/physics3d)
//
// Opt-in: nothing is read or written unless a cache with a folder is given, and the engine never picks one. A key
// is a hash of everything a result is a pure function of (BakeKey: the input's bytes, the options the build reads,
// the format's version and the versions of the libraries that build it), so an entry is never stale: a changed input
// is another key, and the old entry is evicted in time. A file is the key's, its length and a checksum of its bytes:
// one that is truncated, corrupt or of another key is a miss (and removed), never a crash. Writes go to a temporary
// file renamed into place, so another process (or a crash mid-write) never leaves half a file under a key's name.
// The folder is bounded: past `max_bytes` the entries used longest ago go first.

#include <kin/core/types.hpp>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

namespace kin {

// A 64-bit hash of `size` bytes, from `seed`: FNV-1a 64 (the engine's hash for state hashes and keys) in four
// interleaved lanes, each taking 8 bytes of every 32, folded together with the length. Four times plain FNV-1a's speed
// (2.2 GB/s here: a key of the kiln's 540 MB of vertices in a quarter of a second), and as good for telling inputs apart.
u64 bake_hash(const void* data, std::size_t size, u64 seed = 0xcbf29ce484222325ull);

// A BakeCache key: FNV-1a 64 over what a result depends on, fed in order. Each value goes in little-endian at its own
// width, and a run of bytes after its length (as bake_hash of them), so that two inputs cannot run together.
class BakeKey {
public:
    // `kind` names the result (two kinds never share a key); `version` is its format's, raised whenever the bytes or
    // the build that makes them change.
    BakeKey(std::string_view kind, u32 version);

    BakeKey& bytes(const void* data, std::size_t size);
    template <typename T>
        requires std::is_arithmetic_v<T> || std::is_enum_v<T>
    BakeKey& value(T v) {
        u8 b[sizeof(T)];
        std::memcpy(b, &v, sizeof(T));
        if constexpr (std::endian::native == std::endian::big) {
            std::reverse(b, b + sizeof(T));
        }
        for (const u8 x : b) {
            _hash = (_hash ^ x) * 0x100000001b3ull;
        }
        return *this;
    }
    u64 hash() const { return _hash; }

private:
    u64 _hash = 0;
};

// Bytes for a bake: little-endian, fixed width, no padding.
class BakeWriter {
public:
    template <typename T>
        requires std::is_arithmetic_v<T>
    void value(T v) {
        const std::size_t at = _bytes.size();
        _bytes.resize(at + sizeof(T));
        std::memcpy(_bytes.data() + at, &v, sizeof(T));
        if constexpr (std::endian::native == std::endian::big) {
            std::reverse(_bytes.begin() + at, _bytes.end());
        }
    }
    // A count (u64), then the values.
    template <typename T>
        requires std::is_arithmetic_v<T>
    void array(std::span<const T> values) {
        value<u64>(values.size());
        if constexpr (std::endian::native == std::endian::little) {
            raw(values.data(), values.size_bytes());
        } else {
            for (const T v : values) {
                value(v);
            }
        }
    }
    void raw(const void* data, std::size_t size) {
        if (size > 0) {
            const auto* p = static_cast<const u8*>(data);
            _bytes.insert(_bytes.end(), p, p + size);
        }
    }
    void reserve(std::size_t size) { _bytes.reserve(size); }
    std::vector<u8>& bytes() { return _bytes; }

private:
    std::vector<u8> _bytes;
};

// Reads what BakeWriter wrote. Every read checks the bytes left first; a read that fails (too few left, a count past
// what is left) fails the reader, and every read after it fails too, so a caller may read on and test ok() once.
class BakeReader {
public:
    explicit BakeReader(std::span<const u8> bytes) : _bytes(bytes) {}

    template <typename T>
        requires std::is_arithmetic_v<T>
    bool value(T& out) {
        if (!take(sizeof(T))) {
            return false;
        }
        u8 b[sizeof(T)];
        std::memcpy(b, _bytes.data() + _at - sizeof(T), sizeof(T));
        if constexpr (std::endian::native == std::endian::big) {
            std::reverse(b, b + sizeof(T));
        }
        std::memcpy(&out, b, sizeof(T));
        return true;
    }
    // A count written before `element_bytes`-sized elements: refused when they would not fit in the bytes left.
    bool count(u64& out, std::size_t element_bytes) {
        if (!value(out)) {
            return false;
        }
        if (element_bytes > 0 && out > remaining() / element_bytes) {
            _failed = true;
            return false;
        }
        return true;
    }
    template <typename T>
        requires std::is_arithmetic_v<T>
    bool array(std::vector<T>& out) {
        u64 n = 0;
        if (!count(n, sizeof(T))) {
            return false;
        }
        out.resize(static_cast<std::size_t>(n));
        if constexpr (std::endian::native == std::endian::little) {
            return raw(out.data(), out.size() * sizeof(T));
        } else {
            for (T& v : out) {
                value(v);
            }
            return ok();
        }
    }
    bool raw(void* out, std::size_t size) {
        if (!take(size)) {
            return false;
        }
        if (size > 0) {
            std::memcpy(out, _bytes.data() + _at - size, size);
        }
        return true;
    }
    bool ok() const { return !_failed; }
    // Read to the end, and nothing failed.
    bool done() const { return !_failed && _at == _bytes.size(); }
    std::size_t remaining() const { return _bytes.size() - _at; }

private:
    bool take(std::size_t size) {
        if (_failed || size > remaining()) {
            _failed = true;
            return false;
        }
        _at += size;
        return true;
    }
    std::span<const u8> _bytes;
    std::size_t _at = 0;
    bool _failed = false;
};

// What a BakeCache has done since it was made.
struct BakeCacheStats {
    u64 hits = 0;           // reads that gave bytes back
    u64 misses = 0;         // reads that did not (no entry, or one refused)
    u64 refused = 0;        // of the misses, entries there but truncated, corrupt or of another key (removed)
    u64 writes = 0;         // entries written
    u64 write_failures = 0; // writes that could not be made (the folder not writable, the disk full, too big)
    u64 evictions = 0;      // entries removed to keep the folder under max_bytes
    u64 bytes_read = 0;     // the hits' bytes
    u64 bytes_written = 0;  // the writes' bytes
};

// The folder and its entries; safe to use from several threads (and processes) at once. Off when made without a folder.
class BakeCache {
public:
    static constexpr u64 default_max_bytes = 1024ull * 1024ull * 1024ull; // 1 GiB

    // Off: every read a miss, every write dropped (nothing is counted).
    BakeCache() = default;
    // Entries in `directory`, made by the first write if it does not exist. The engine never picks a folder by itself:
    // a game passes one, by convention `kin::user_data_dir(app) / "kin" / "bake"`.
    explicit BakeCache(std::filesystem::path directory, u64 max_bytes = default_max_bytes);
    BakeCache(const BakeCache&) = delete;
    BakeCache& operator=(const BakeCache&) = delete;

    bool enabled() const { return !_directory.empty(); }
    const std::filesystem::path& directory() const { return _directory; }
    u64 max_bytes() const { return _max_bytes; }
    // The bytes written under `key`, or none. A hit marks the entry used, for the eviction order.
    std::optional<std::vector<u8>> read(u64 key);
    // Keeps `bytes` under `key` (replacing what was there), then evicts the entries used longest ago while the folder
    // holds more than max_bytes. A failure is counted (and logged once), never thrown; bytes larger than max_bytes are
    // not kept.
    void write(u64 key, std::span<const u8> bytes);
    // The file an entry is kept in.
    std::filesystem::path path_of(u64 key) const;
    BakeCacheStats stats() const;

private:
    void evict(const std::filesystem::path& keep);
    std::filesystem::path _directory;
    u64 _max_bytes = default_max_bytes;
    mutable std::mutex _mutex;
    BakeCacheStats _stats{};
    bool _warned = false;
};

} // namespace kin
