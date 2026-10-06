#include <kin/assets/content.hpp>

#include <kin/platform/log.hpp>

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <shared_mutex>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "content_io.hpp"

namespace kin {

static_assert(std::endian::native == std::endian::little, "kin packs are read in place as little-endian");

namespace {

// A pack is a header, the files' bytes (each starting on a 16-byte boundary)
// and an index after them:
//
//   header  magic[8] "KINPAK\x1A\n"; u32 version; u32 entry count;
//           u64 index offset; u32 index size; u32 index CRC-32
//   index   per entry, in path order: u64 offset; u64 size; u32 CRC-32;
//           u32 path length; path (UTF-8, '/'-separated, relative)
constexpr std::array<char, 8> PackMagic{'K', 'I', 'N', 'P', 'A', 'K', '\x1A', '\n'};
constexpr u32 PackVersion = 1;
constexpr std::size_t HeaderSize = 32;
constexpr std::size_t EntryFixedSize = 24;
constexpr u64 FileAlignment = 16;

const std::array<u32, 256>& crc_table() {
    static const std::array<u32, 256> table = [] {
        std::array<u32, 256> result{};
        for (u32 i = 0; i < 256; ++i) {
            u32 c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            result[i] = c;
        }
        return result;
    }();
    return table;
}

u32 crc32_update(u32 crc, const std::byte* data, std::size_t size) {
    const auto& table = crc_table();
    crc = ~crc;
    for (std::size_t i = 0; i < size; ++i) {
        crc = table[(crc ^ std::to_integer<u32>(data[i])) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

template<typename T>
T read_le(const std::byte* at) {
    T value;
    std::memcpy(&value, at, sizeof(T));
    return value;
}

template<typename T>
void append_le(std::string& out, T value) {
    char bytes[sizeof(T)];
    std::memcpy(bytes, &value, sizeof(T));
    out.append(bytes, sizeof(T));
}

std::string utf8(const std::filesystem::path& path) {
    const std::u8string text = path.generic_u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

// A path as SDL takes it: UTF-8, with the platform's separators.
std::string utf8_native(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

std::filesystem::path from_utf8(std::string_view text) {
    return std::filesystem::path{std::u8string{reinterpret_cast<const char8_t*>(text.data()), text.size()}};
}

// A relative, '/'-separated path that stays inside the pack.
bool valid_entry_path(std::string_view path) {
    if (path.empty() || path.front() == '/' || path.back() == '/' || path.find('\\') != std::string_view::npos ||
        path.find('\0') != std::string_view::npos) {
        return false;
    }
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t end = std::min(path.find('/', start), path.size());
        const std::string_view part = path.substr(start, end - start);
        if (part.empty() || part == "." || part == "..") {
            return false;
        }
        start = end + 1;
    }
    return true;
}

bool equal_ignoring_case(std::string_view a, std::string_view b) {
    return std::ranges::equal(a, b, [](char x, char y) {
        const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
        return lower(x) == lower(y);
    });
}

std::filesystem::path normalized(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::path absolute = std::filesystem::absolute(path, error);
    std::filesystem::path result = (error ? path : absolute).lexically_normal();
    if (!result.has_filename() && result.has_relative_path()) {
        result = result.parent_path(); // "a/b/" is the folder "a/b"
    }
    return result;
}

void set_error(std::string* error, std::string message) {
    if (error) {
        *error = std::move(message);
    }
}

std::optional<std::string> read_disk_file(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) {
        return std::nullopt;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    std::string bytes;
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (!error) {
        bytes.reserve(static_cast<std::size_t>(size));
    }
    bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    if (file.bad()) {
        return std::nullopt;
    }
    return bytes;
}

struct Mount {
    std::filesystem::path point; // normalized, absolute
    std::shared_ptr<const ContentPack> pack;
};

struct Mounts {
    std::shared_mutex mutex;
    std::vector<Mount> list; // longest mount point first, so nested mounts win
};

Mounts& mounts() {
    static Mounts state;
    return state;
}

// Where `path` is in a mounted pack: the pack, and the path inside it ("" for
// the mount point itself).
struct PackLocation {
    std::shared_ptr<const ContentPack> pack;
    std::string inner;
};

std::optional<PackLocation> locate(const std::filesystem::path& path) {
    Mounts& state = mounts();
    std::shared_lock lock{state.mutex};
    if (state.list.empty()) {
        return std::nullopt;
    }
    const std::filesystem::path query = normalized(path);
    for (const Mount& mount : state.list) {
        const std::filesystem::path relative = query.lexically_relative(mount.point);
        if (relative.empty()) {
            continue;
        }
        const std::string inner = utf8(relative);
        if (inner == "..") {
            continue;
        }
        if (inner.starts_with("../")) {
            continue;
        }
        return PackLocation{mount.pack, inner == "." ? std::string{} : inner};
    }
    return std::nullopt;
}

const ContentPack::Entry* find_entry(const PackLocation& location) {
    const ContentPack::Entry* entry = location.pack->find(location.inner);
    if (!entry && !location.inner.empty()) {
        if (const ContentPack::Entry* similar = location.pack->find_ignoring_case(location.inner)) {
            // Found on a case-insensitive disk while it was made, missing now.
            KIN_LOG_ERROR_F("content", "content file not found: letter case differs from the packed file",
                            (LogFields{{.name = "path", .value = location.inner},
                                       {.name = "packed", .value = similar->path},
                                       {.name = "pack", .value = location.pack->file().string()}}));
        }
    }
    return entry;
}

struct FoundRoot {
    std::mutex mutex;
    std::filesystem::path root;
};

FoundRoot& found_root() {
    static FoundRoot state;
    return state;
}

} // namespace

// --- ContentPack -----------------------------------------------------------

ContentPack::~ContentPack() {
#if defined(_WIN32)
    if (_data) {
        UnmapViewOfFile(_data);
    }
    if (_mapping) {
        CloseHandle(static_cast<HANDLE>(_mapping));
    }
#else
    if (_data) {
        munmap(const_cast<std::byte*>(_data), _size);
    }
#endif
}

std::shared_ptr<const ContentPack> ContentPack::open(const std::filesystem::path& file, std::string* error) {
    std::shared_ptr<ContentPack> pack{new ContentPack};
    pack->_file = file;
    const std::string name = file.string();

#if defined(_WIN32)
    HANDLE handle = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        set_error(error, "cannot open " + name);
        return nullptr;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle, &size) || size.QuadPart < static_cast<LONGLONG>(HeaderSize)) {
        CloseHandle(handle);
        set_error(error, name + " is not a kin pack (too short)");
        return nullptr;
    }
    HANDLE mapping = CreateFileMappingW(handle, nullptr, PAGE_READONLY, 0, 0, nullptr);
    CloseHandle(handle);
    if (!mapping) {
        set_error(error, "cannot map " + name);
        return nullptr;
    }
    pack->_mapping = mapping;
    pack->_data = static_cast<const std::byte*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
    pack->_size = static_cast<std::size_t>(size.QuadPart);
    if (!pack->_data) {
        set_error(error, "cannot map " + name);
        return nullptr;
    }
#else
    const int fd = ::open(file.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        set_error(error, "cannot open " + name);
        return nullptr;
    }
    struct stat info {};
    if (fstat(fd, &info) != 0 || info.st_size < static_cast<off_t>(HeaderSize)) {
        ::close(fd);
        set_error(error, name + " is not a kin pack (too short)");
        return nullptr;
    }
    void* data = mmap(nullptr, static_cast<std::size_t>(info.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (data == MAP_FAILED) {
        set_error(error, "cannot map " + name);
        return nullptr;
    }
    pack->_data = static_cast<const std::byte*>(data);
    pack->_size = static_cast<std::size_t>(info.st_size);
#endif

    const std::byte* base = pack->_data;
    if (std::memcmp(base, PackMagic.data(), PackMagic.size()) != 0) {
        set_error(error, name + " is not a kin pack");
        return nullptr;
    }
    const u32 version = read_le<u32>(base + 8);
    if (version != PackVersion) {
        set_error(error, name + " is pack version " + std::to_string(version) + "; this build reads version " +
                             std::to_string(PackVersion));
        return nullptr;
    }
    const u32 count = read_le<u32>(base + 12);
    const u64 index_offset = read_le<u64>(base + 16);
    const u32 index_size = read_le<u32>(base + 24);
    const u32 index_crc = read_le<u32>(base + 28);
    if (index_offset < HeaderSize || index_offset > pack->_size || index_size > pack->_size - index_offset) {
        set_error(error, name + " is damaged (index out of range)");
        return nullptr;
    }
    const std::byte* index = base + index_offset;
    if (crc32_update(0, index, index_size) != index_crc) {
        set_error(error, name + " is damaged (index checksum)");
        return nullptr;
    }
    pack->_entries.reserve(count);
    std::size_t at = 0;
    for (u32 i = 0; i < count; ++i) {
        if (index_size - at < EntryFixedSize) {
            set_error(error, name + " is damaged (index too short)");
            return nullptr;
        }
        Entry entry;
        entry.offset = read_le<u64>(index + at);
        entry.size = read_le<u64>(index + at + 8);
        entry.crc32 = read_le<u32>(index + at + 16);
        const u32 length = read_le<u32>(index + at + 20);
        at += EntryFixedSize;
        if (index_size - at < length) {
            set_error(error, name + " is damaged (index too short)");
            return nullptr;
        }
        entry.path.assign(reinterpret_cast<const char*>(index + at), length);
        at += length;
        if (!valid_entry_path(entry.path)) {
            set_error(error, name + " has an invalid path: " + entry.path);
            return nullptr;
        }
        if (entry.offset < HeaderSize || entry.offset > index_offset || entry.size > index_offset - entry.offset) {
            set_error(error, name + " is damaged (" + entry.path + " out of range)");
            return nullptr;
        }
        if (!pack->_entries.empty() && pack->_entries.back().path >= entry.path) {
            set_error(error, name + " is damaged (index out of order at " + entry.path + ")");
            return nullptr;
        }
        pack->_entries.push_back(std::move(entry));
    }
    return pack;
}

const ContentPack::Entry* ContentPack::find(std::string_view path) const {
    const auto found = std::ranges::lower_bound(_entries, path, {}, [](const Entry& e) -> std::string_view {
        return e.path;
    });
    return found != _entries.end() && found->path == path ? &*found : nullptr;
}

const ContentPack::Entry* ContentPack::find_ignoring_case(std::string_view path) const {
    const auto found = std::ranges::find_if(_entries, [&](const Entry& e) { return equal_ignoring_case(e.path, path); });
    return found != _entries.end() ? &*found : nullptr;
}

std::span<const std::byte> ContentPack::bytes(const Entry& entry) const {
    return {_data + entry.offset, static_cast<std::size_t>(entry.size)};
}

bool ContentPack::verify(std::string* error) const {
    for (const Entry& entry : _entries) {
        const std::span<const std::byte> data = bytes(entry);
        if (crc32_update(0, data.data(), data.size()) != entry.crc32) {
            set_error(error, entry.path + " does not match its checksum");
            return false;
        }
    }
    return true;
}

// --- Writing ---------------------------------------------------------------

ContentPackWriteResult write_content_pack(const std::filesystem::path& dir, const std::filesystem::path& out) {
    ContentPackWriteResult result;
    std::error_code error;
    if (!std::filesystem::is_directory(dir, error)) {
        result.error = dir.string() + " is not a folder";
        return result;
    }

    struct Source {
        std::string path;
        std::filesystem::path file;
    };
    std::vector<Source> sources;
    for (auto it = std::filesystem::recursive_directory_iterator(dir, error);
         !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
        const std::string name = utf8(it->path().filename());
        if (name.starts_with('.')) {
            if (it->is_directory(error)) {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (!it->is_regular_file(error)) {
            continue;
        }
        const std::string path = utf8(it->path().lexically_relative(dir));
        if (!valid_entry_path(path)) {
            result.error = "cannot pack " + it->path().string() + ": its path cannot be stored";
            return result;
        }
        sources.push_back({path, it->path()});
    }
    if (error) {
        result.error = "cannot list " + dir.string() + ": " + error.message();
        return result;
    }
    std::ranges::sort(sources, {}, &Source::path);

    std::filesystem::create_directories(out.parent_path().empty() ? "." : out.parent_path(), error);
    std::filesystem::path temp = out;
    temp += ".partial";
    std::ofstream file(temp, std::ios::binary | std::ios::trunc);
    if (!file) {
        result.error = "cannot write " + temp.string();
        return result;
    }

    std::string header(HeaderSize, '\0');
    file.write(header.data(), static_cast<std::streamsize>(header.size()));
    u64 offset = HeaderSize;
    std::string index;
    for (const Source& source : sources) {
        const std::optional<std::string> bytes = read_disk_file(source.file);
        if (!bytes) {
            result.error = "cannot read " + source.file.string();
            file.close();
            std::filesystem::remove(temp, error);
            return result;
        }
        const u64 padding = (FileAlignment - offset % FileAlignment) % FileAlignment;
        const std::string zeros(static_cast<std::size_t>(padding), '\0');
        file.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
        offset += padding;
        file.write(bytes->data(), static_cast<std::streamsize>(bytes->size()));

        append_le<u64>(index, offset);
        append_le<u64>(index, bytes->size());
        append_le<u32>(index, crc32_update(0, reinterpret_cast<const std::byte*>(bytes->data()), bytes->size()));
        append_le<u32>(index, static_cast<u32>(source.path.size()));
        index += source.path;
        offset += bytes->size();
        result.bytes += bytes->size();
    }
    file.write(index.data(), static_cast<std::streamsize>(index.size()));

    header.clear();
    header.append(PackMagic.data(), PackMagic.size());
    append_le<u32>(header, PackVersion);
    append_le<u32>(header, static_cast<u32>(sources.size()));
    append_le<u64>(header, offset);
    append_le<u32>(header, static_cast<u32>(index.size()));
    append_le<u32>(header, crc32_update(0, reinterpret_cast<const std::byte*>(index.data()), index.size()));
    file.seekp(0);
    file.write(header.data(), static_cast<std::streamsize>(header.size()));
    file.close();
    if (!file) {
        result.error = "cannot write " + temp.string();
        std::filesystem::remove(temp, error);
        return result;
    }
    std::filesystem::rename(temp, out, error);
    if (error) {
        result.error = "cannot replace " + out.string() + ": " + error.message();
        std::filesystem::remove(temp, error);
        return result;
    }
    result.ok = true;
    result.files = sources.size();
    return result;
}

// --- Mounts and reads ------------------------------------------------------

void mount_content_pack(const std::filesystem::path& mount_point, std::shared_ptr<const ContentPack> pack) {
    Mounts& state = mounts();
    std::unique_lock lock{state.mutex};
    const std::filesystem::path point = normalized(mount_point);
    std::erase_if(state.list, [&](const Mount& m) { return m.point == point; });
    state.list.push_back({point, std::move(pack)});
    std::ranges::stable_sort(state.list, std::greater<>{}, [](const Mount& m) { return m.point.native().size(); });
}

void unmount_content_pack(const std::filesystem::path& mount_point) {
    Mounts& state = mounts();
    std::unique_lock lock{state.mutex};
    const std::filesystem::path point = normalized(mount_point);
    std::erase_if(state.list, [&](const Mount& m) { return m.point == point; });
}

void unmount_all_content_packs() {
    Mounts& state = mounts();
    std::unique_lock lock{state.mutex};
    state.list.clear();
}

std::shared_ptr<const ContentPack> content_pack_for(const std::filesystem::path& path) {
    std::optional<PackLocation> location = locate(path);
    return location ? location->pack : nullptr;
}

std::optional<std::string> read_content_file(const std::filesystem::path& path) {
    if (const std::optional<PackLocation> location = locate(path)) {
        const ContentPack::Entry* entry = find_entry(*location);
        if (!entry) {
            return std::nullopt;
        }
        const std::span<const std::byte> bytes = location->pack->bytes(*entry);
        return std::string{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
    }
    return read_disk_file(path);
}

std::optional<std::string> read_content_text(const std::filesystem::path& path) {
    std::optional<std::string> text = read_content_file(path);
    if (text && text->find('\r') != std::string::npos) {
        std::string lf;
        lf.reserve(text->size());
        for (std::size_t i = 0; i < text->size(); ++i) {
            if ((*text)[i] != '\r' || i + 1 >= text->size() || (*text)[i + 1] != '\n') {
                lf += (*text)[i];
            }
        }
        *text = std::move(lf);
    }
    return text;
}

std::optional<ContentBytes> view_content_file(const std::filesystem::path& path) {
    if (const std::optional<PackLocation> location = locate(path)) {
        const ContentPack::Entry* entry = find_entry(*location);
        if (!entry) {
            return std::nullopt;
        }
        return ContentBytes{location->pack->bytes(*entry), location->pack};
    }
    std::optional<std::string> bytes = read_disk_file(path);
    if (!bytes) {
        return std::nullopt;
    }
    auto owner = std::make_shared<const std::string>(std::move(*bytes));
    return ContentBytes{std::as_bytes(std::span{owner->data(), owner->size()}), owner};
}

bool content_file_exists(const std::filesystem::path& path) {
    if (const std::optional<PackLocation> location = locate(path)) {
        return !location->inner.empty() && location->pack->find(location->inner);
    }
    std::error_code error;
    return std::filesystem::is_regular_file(path, error);
}

bool content_directory_exists(const std::filesystem::path& path) {
    if (const std::optional<PackLocation> location = locate(path)) {
        if (location->inner.empty()) {
            return true;
        }
        const std::string prefix = location->inner + "/";
        const auto& entries = location->pack->entries();
        const auto found = std::ranges::lower_bound(entries, prefix, {}, [](const ContentPack::Entry& e) -> std::string_view {
            return e.path;
        });
        return found != entries.end() && found->path.starts_with(prefix);
    }
    std::error_code error;
    return std::filesystem::is_directory(path, error);
}

std::optional<u64> content_file_size(const std::filesystem::path& path) {
    if (const std::optional<PackLocation> location = locate(path)) {
        const ContentPack::Entry* entry = location->inner.empty() ? nullptr : location->pack->find(location->inner);
        return entry ? std::optional<u64>{entry->size} : std::nullopt;
    }
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) {
        return std::nullopt;
    }
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    return error ? std::nullopt : std::optional<u64>{size};
}

std::vector<std::filesystem::path> list_content_files(const std::filesystem::path& dir, bool recursive) {
    std::vector<std::filesystem::path> files;
    if (const std::optional<PackLocation> location = locate(dir)) {
        const std::string prefix = location->inner.empty() ? std::string{} : location->inner + "/";
        const auto& entries = location->pack->entries();
        auto it = std::ranges::lower_bound(entries, prefix, {}, [](const ContentPack::Entry& e) -> std::string_view {
            return e.path;
        });
        for (; it != entries.end() && it->path.starts_with(prefix); ++it) {
            const std::string_view rest = std::string_view{it->path}.substr(prefix.size());
            if (!recursive && rest.find('/') != std::string_view::npos) {
                continue;
            }
            files.push_back(dir / from_utf8(rest));
        }
        return files;
    }
    std::error_code error;
    if (!std::filesystem::is_directory(dir, error)) {
        return files;
    }
    const auto add = [&](const std::filesystem::directory_entry& entry) {
        std::error_code ignored;
        if (entry.is_regular_file(ignored)) {
            files.push_back(entry.path());
        }
    };
    if (recursive) {
        for (auto it = std::filesystem::recursive_directory_iterator(dir, error);
             !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
            add(*it);
        }
    } else {
        for (auto it = std::filesystem::directory_iterator(dir, error);
             !error && it != std::filesystem::directory_iterator(); it.increment(error)) {
            add(*it);
        }
    }
    std::ranges::sort(files);
    return files;
}

// --- SDL streams -----------------------------------------------------------

namespace {

struct ContentStream {
    ContentBytes content;
    std::size_t at = 0;
};

Sint64 SDLCALL stream_size(void* data) {
    return static_cast<Sint64>(static_cast<ContentStream*>(data)->content.bytes.size());
}

Sint64 SDLCALL stream_seek(void* data, Sint64 offset, SDL_IOWhence whence) {
    auto* stream = static_cast<ContentStream*>(data);
    const auto size = static_cast<Sint64>(stream->content.bytes.size());
    Sint64 from = 0;
    if (whence == SDL_IO_SEEK_CUR) {
        from = static_cast<Sint64>(stream->at);
    } else if (whence == SDL_IO_SEEK_END) {
        from = size;
    }
    const Sint64 to = from + offset;
    if (to < 0) {
        SDL_SetError("content stream: seek before the start");
        return -1;
    }
    stream->at = static_cast<std::size_t>(std::min(to, size));
    return static_cast<Sint64>(stream->at);
}

std::size_t SDLCALL stream_read(void* data, void* out, std::size_t size, SDL_IOStatus* status) {
    auto* stream = static_cast<ContentStream*>(data);
    const std::size_t left = stream->content.bytes.size() - stream->at;
    const std::size_t count = std::min(size, left);
    std::memcpy(out, stream->content.bytes.data() + stream->at, count);
    stream->at += count;
    if (count < size) {
        *status = SDL_IO_STATUS_EOF;
    }
    return count;
}

bool SDLCALL stream_close(void* data) {
    delete static_cast<ContentStream*>(data);
    return true;
}

} // namespace

SDL_IOStream* open_content_stream(const std::filesystem::path& path) {
    if (!locate(path)) {
        // On disk: SDL reads it as it goes (a large font is not read whole).
        return SDL_IOFromFile(utf8_native(path).c_str(), "rb");
    }
    std::optional<ContentBytes> content = view_content_file(path);
    if (!content) {
        SDL_SetError("content file not found: %s", path.string().c_str());
        return nullptr;
    }
    SDL_IOStreamInterface iface;
    SDL_INIT_INTERFACE(&iface);
    iface.size = stream_size;
    iface.seek = stream_seek;
    iface.read = stream_read;
    iface.close = stream_close;
    auto* stream = new ContentStream{std::move(*content)};
    SDL_IOStream* io = SDL_OpenIO(&iface, stream);
    if (!io) {
        delete stream;
    }
    return io;
}

// --- Finding the content ---------------------------------------------------

std::filesystem::path executable_dir() {
    if (const char* base = SDL_GetBasePath()) {
        return std::filesystem::path{std::u8string{reinterpret_cast<const char8_t*>(base)}}.lexically_normal();
    }
    std::error_code error;
    return std::filesystem::current_path(error);
}

namespace {

// Uses `candidate` (a folder or a pack) as the content; empty if it is neither.
std::filesystem::path use_content(const std::filesystem::path& candidate, std::string_view source) {
    std::error_code error;
    if (std::filesystem::is_directory(candidate, error)) {
        const std::filesystem::path root = normalized(candidate);
        KIN_LOG_INFO_F("content", "content folder",
                       (LogFields{{.name = "root", .value = root.string()}, {.name = "from", .value = std::string{source}}}));
        return root;
    }
    if (std::filesystem::is_regular_file(candidate, error)) {
        std::string why;
        std::shared_ptr<const ContentPack> pack = ContentPack::open(candidate, &why);
        if (!pack) {
            KIN_LOG_ERROR_F("content", "content pack unreadable",
                            (LogFields{{.name = "pack", .value = candidate.string()}, {.name = "error", .value = why}}));
            return {};
        }
        std::filesystem::path root = normalized(candidate);
        root.replace_extension();
        KIN_LOG_INFO_F("content", "content pack",
                       (LogFields{{.name = "pack", .value = candidate.string()},
                                  {.name = "root", .value = root.string()},
                                  {.name = "files", .value = std::to_string(pack->entries().size())},
                                  {.name = "from", .value = std::string{source}}}));
        mount_content_pack(root, std::move(pack));
        return root;
    }
    return {};
}

std::filesystem::path remember(std::filesystem::path root) {
    FoundRoot& state = found_root();
    std::lock_guard lock{state.mutex};
    state.root = root;
    return root;
}

} // namespace

std::filesystem::path find_content_root(const ContentSearch& search, int argc, char** argv) {
    // Arguments and variables are in the system's encoding, as std::filesystem
    // takes a narrow string.
    std::optional<std::filesystem::path> override_path;
    std::string_view override_source;
    for (int i = 1; argv && i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg.starts_with("--content=")) {
            override_path = std::filesystem::path{std::string{arg.substr(10)}};
            override_source = "--content";
        } else if (arg == "--content" && i + 1 < argc) {
            override_path = std::filesystem::path{argv[++i]};
            override_source = "--content";
        }
    }
    if (!override_path) {
        if (const char* env = std::getenv("KIN_CONTENT"); env && *env) {
            override_path = std::filesystem::path{env};
            override_source = "KIN_CONTENT";
        }
    }
    if (override_path) {
        std::filesystem::path root = use_content(*override_path, override_source);
        if (root.empty()) {
            KIN_LOG_ERROR_F("content", "content not found",
                            (LogFields{{.name = "path", .value = override_path->string()},
                                       {.name = "from", .value = std::string{override_source}}}));
        }
        return remember(std::move(root));
    }

    std::vector<std::string> tried;
    if (!search.dev_dir.empty()) {
        if (std::filesystem::path root = use_content(search.dev_dir, "source folder"); !root.empty()) {
            return remember(std::move(root));
        }
        tried.push_back(search.dev_dir.string());
    }
    const std::filesystem::path beside = executable_dir();
    const std::filesystem::path pack = beside / (search.name + ".kinpak");
    std::error_code error;
    if (std::filesystem::is_regular_file(pack, error)) {
        return remember(use_content(pack, "beside the executable"));
    }
    tried.push_back(pack.string());
    if (std::filesystem::path root = use_content(beside / search.name, "beside the executable"); !root.empty()) {
        return remember(std::move(root));
    }
    tried.push_back((beside / search.name).string());

    std::string looked;
    for (const std::string& path : tried) {
        looked += looked.empty() ? path : ", " + path;
    }
    KIN_LOG_ERROR_F("content", "content not found",
                    (LogFields{{.name = "name", .value = search.name}, {.name = "tried", .value = looked}}));
    return remember({});
}

std::filesystem::path content_root() {
    FoundRoot& state = found_root();
    std::lock_guard lock{state.mutex};
    return state.root;
}

} // namespace kin
