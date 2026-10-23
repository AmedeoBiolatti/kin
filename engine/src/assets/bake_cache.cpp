#include <kin/assets/bake_cache.hpp>

#include <kin/platform/log.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <functional>
#include <string>
#include <thread>

namespace kin {

namespace {

namespace fs = std::filesystem;

// A file: the magic (the file layout's version is its last byte), the key, the payload's length and its FNV-1a 64,
// all u64 little-endian, then the payload.
constexpr u64 file_magic = 0x31454b41424e494bull; // "KINBAKE1"
constexpr std::size_t header_bytes = 32;

std::string hex16(u64 v) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string s(16, '0');
    for (int i = 15; i >= 0; --i) {
        s[static_cast<std::size_t>(i)] = digits[v & 15u];
        v >>= 4;
    }
    return s;
}

std::array<u8, header_bytes> header_of(u64 key, std::span<const u8> payload) {
    BakeWriter w;
    w.value(file_magic);
    w.value(key);
    w.value(static_cast<u64>(payload.size()));
    w.value(bake_hash(payload.data(), payload.size()));
    std::array<u8, header_bytes> out{};
    std::memcpy(out.data(), w.bytes().data(), header_bytes);
    return out;
}

// A name for a temporary file no other writer (thread or process) uses at the same moment.
u64 unique_number() {
    static std::atomic<u64> counter{0};
    static const u64 process = [] {
        static int anchor = 0;
        const u64 a = static_cast<u64>(reinterpret_cast<std::uintptr_t>(&anchor));
        const u64 t = static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
        return bake_hash(&t, sizeof(t), a * 0x9e3779b97f4a7c15ull);
    }();
    const u64 n = counter.fetch_add(1, std::memory_order_relaxed);
    const u64 thread = static_cast<u64>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    return bake_hash(&n, sizeof(n), process ^ thread);
}

} // namespace

u64 bake_hash(const void* data, std::size_t size, u64 seed) {
    constexpr u64 prime = 0x100000001b3ull;
    const auto* p = static_cast<const u8*>(data);
    // One FNV-1a chain is a multiply after every byte, each waiting for the one before; four chains keep the
    // multiplier busy.
    u64 a = seed, b = seed ^ 1u, c = seed ^ 2u, d = seed ^ 3u;
    std::size_t i = 0;
    for (; i + 32 <= size; i += 32) {
        for (std::size_t k = 0; k < 8; ++k) {
            a = (a ^ p[i + k]) * prime;
            b = (b ^ p[i + 8 + k]) * prime;
            c = (c ^ p[i + 16 + k]) * prime;
            d = (d ^ p[i + 24 + k]) * prime;
        }
    }
    for (; i < size; ++i) {
        a = (a ^ p[i]) * prime;
    }
    u64 h = seed;
    for (const u64 lane : {a, b, c, d, static_cast<u64>(size)}) {
        for (int k = 0; k < 8; ++k) {
            h = (h ^ ((lane >> (8 * k)) & 0xffu)) * prime;
        }
    }
    return h;
}

BakeKey::BakeKey(std::string_view kind, u32 version) {
    _hash = 0xcbf29ce484222325ull;
    bytes(kind.data(), kind.size());
    value(version);
}

BakeKey& BakeKey::bytes(const void* data, std::size_t size) {
    value(static_cast<u64>(size));
    _hash = bake_hash(data, size, _hash);
    return *this;
}

BakeCache::BakeCache(fs::path directory, u64 max_bytes) : _directory(std::move(directory)), _max_bytes(max_bytes) {}

fs::path BakeCache::path_of(u64 key) const {
    return _directory / (hex16(key) + ".bake");
}

std::optional<std::vector<u8>> BakeCache::read(u64 key) {
    if (!enabled()) {
        return std::nullopt;
    }
    const fs::path path = path_of(key);
    std::error_code error;
    const u64 size = fs::file_size(path, error);
    if (error) {
        std::lock_guard lock{_mutex};
        ++_stats.misses;
        return std::nullopt;
    }
    // Every length is checked against the file's before anything is read or allocated.
    std::optional<std::vector<u8>> payload;
    if (size >= header_bytes && size - header_bytes <= _max_bytes) {
        std::ifstream in(path, std::ios::binary);
        std::array<u8, header_bytes> header{};
        if (in.read(reinterpret_cast<char*>(header.data()), header_bytes)) {
            BakeReader r{header};
            u64 magic = 0, stored_key = 0, length = 0, checksum = 0;
            r.value(magic);
            r.value(stored_key);
            r.value(length);
            r.value(checksum);
            if (r.done() && magic == file_magic && stored_key == key && length == size - header_bytes) {
                std::vector<u8> bytes(static_cast<std::size_t>(length));
                if (in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(length)) &&
                    static_cast<u64>(in.gcount()) == length && bake_hash(bytes.data(), bytes.size()) == checksum) {
                    payload = std::move(bytes);
                }
            }
        }
    }
    if (!payload) {
        // Truncated (a write cut short by a crash or a full disk), corrupt or not ours: gone, so the next write
        // replaces it.
        fs::remove(path, error);
        std::lock_guard lock{_mutex};
        ++_stats.misses;
        ++_stats.refused;
        return std::nullopt;
    }
    // Used now: the eviction takes the entries used longest ago first (a read-only folder keeps its order).
    fs::last_write_time(path, fs::file_time_type::clock::now(), error);
    std::lock_guard lock{_mutex};
    ++_stats.hits;
    _stats.bytes_read += payload->size();
    return payload;
}

void BakeCache::write(u64 key, std::span<const u8> bytes) {
    if (!enabled()) {
        return;
    }
    const auto fail = [&](const char* why) {
        std::lock_guard lock{_mutex};
        ++_stats.write_failures;
        if (!_warned) {
            _warned = true;
            KIN_LOG_WARN_F("assets", "bake cache: an entry was not written (later failures are not logged)",
                           (LogFields{{.name = "reason", .value = std::string{why}}, {.name = "directory", .value = _directory.string()}}));
        }
    };
    if (bytes.size() > _max_bytes - std::min<u64>(_max_bytes, header_bytes)) {
        fail("larger than the cache's max_bytes");
        return;
    }
    std::error_code error;
    fs::create_directories(_directory, error);
    const fs::path path = path_of(key);
    const fs::path temporary = _directory / (hex16(key) + "." + hex16(unique_number()) + ".tmp");
    bool written = false;
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (out) {
            const std::array<u8, header_bytes> header = header_of(key, bytes);
            out.write(reinterpret_cast<const char*>(header.data()), header_bytes);
            out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            out.close();
            written = !out.fail();
        }
    }
    if (written) {
        // Atomic where the folder is on one file system: a reader sees the old entry or the new one, whole.
        fs::rename(temporary, path, error);
        written = !error;
    }
    if (!written) {
        fs::remove(temporary, error);
        fail("the file could not be written or renamed into place");
        return;
    }
    std::lock_guard lock{_mutex};
    ++_stats.writes;
    _stats.bytes_written += bytes.size();
    evict(path);
}

void BakeCache::evict(const fs::path& keep) {
    struct Entry {
        fs::path path;
        u64 size = 0;
        fs::file_time_type time{};
    };
    std::vector<Entry> entries;
    u64 total = 0;
    const fs::file_time_type now = fs::file_time_type::clock::now();
    std::error_code error;
    for (fs::directory_iterator it{_directory, error}, end; !error && it != end; it.increment(error)) {
        const fs::path& p = it->path();
        const fs::path extension = p.extension();
        if (extension != ".bake" && extension != ".tmp") {
            continue;
        }
        std::error_code e;
        const u64 size = it->file_size(e);
        const fs::file_time_type time = e ? now : it->last_write_time(e);
        if (e) {
            continue;
        }
        if (extension == ".tmp") {
            // Left by a write that never finished (a crash): another writer's is minutes old at most.
            if (now - time > std::chrono::hours{1}) {
                fs::remove(p, e);
                continue;
            }
        }
        total += size;
        entries.push_back({p, size, time});
    }
    if (total <= _max_bytes) {
        return;
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.time != b.time ? a.time < b.time : a.path < b.path; });
    for (const Entry& e : entries) {
        if (total <= _max_bytes) {
            break;
        }
        if (e.path == keep) {
            continue;
        }
        std::error_code removed;
        if (fs::remove(e.path, removed) && !removed) {
            total -= e.size;
            ++_stats.evictions;
        }
    }
}

BakeCacheStats BakeCache::stats() const {
    std::lock_guard lock{_mutex};
    return _stats;
}

} // namespace kin
