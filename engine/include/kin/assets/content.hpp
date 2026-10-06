#pragma once

#include <kin/core/types.hpp>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kin {

// A game's content (images, sounds, fonts, scripts, data files) is a folder
// while it is made and, when it ships, either the same folder beside the
// executable or one .kinpak archive standing in for it. The engine reads every
// content file through the functions below, so the same paths work either way:
//
//   const std::filesystem::path root = kin::find_content_root(KIN_GAME_CONTENT, argc, argv);
//   kin::AssetManager assets{root};      // root/hero.png, from the folder or the pack
//
// A mounted pack answers for every path under its mount point; any other path
// is read from disk.

// One .kinpak archive, read-only and memory-mapped. Its files are kept in path
// order; a path is relative to the pack's root, separated by '/'.
class ContentPack {
public:
    struct Entry {
        std::string path;
        u64 offset = 0;
        u64 size = 0;
        u32 crc32 = 0;
    };

    // Opens and checks the index of `file`; null, with the reason in `error`,
    // if it is not a pack this build can read.
    static std::shared_ptr<const ContentPack> open(const std::filesystem::path& file, std::string* error = nullptr);

    ContentPack(const ContentPack&) = delete;
    ContentPack& operator=(const ContentPack&) = delete;
    ~ContentPack();

    const std::filesystem::path& file() const { return _file; }
    const std::vector<Entry>& entries() const { return _entries; }
    // The entry at exactly `path`, or null.
    const Entry* find(std::string_view path) const;
    // The entry whose path differs from `path` only in letter case, or null.
    const Entry* find_ignoring_case(std::string_view path) const;
    // An entry's bytes, valid while this pack lives.
    std::span<const std::byte> bytes(const Entry& entry) const;
    // Checks every file against its checksum; false, naming the first bad
    // file in `error`, if one does not match.
    bool verify(std::string* error = nullptr) const;

private:
    ContentPack() = default;

    std::filesystem::path _file;
    std::vector<Entry> _entries;
    const std::byte* _data = nullptr;
    std::size_t _size = 0;
    [[maybe_unused]] void* _mapping = nullptr; // the mapping's handle, on Windows
};

struct ContentPackWriteResult {
    bool ok = false;
    std::size_t files = 0;
    u64 bytes = 0;
    std::string error;
};

// Writes every file under `dir` into a new pack at `out`. Files and folders
// whose names start with '.' are left out. The pack is written beside `out`
// and renamed into place, so a failed write leaves any old pack as it was.
ContentPackWriteResult write_content_pack(const std::filesystem::path& dir, const std::filesystem::path& out);

// Makes `pack` stand in for the folder `mount_point`: content reads under that
// folder come from the pack, whether or not the folder exists. Mounting again
// at the same point replaces the pack there.
void mount_content_pack(const std::filesystem::path& mount_point, std::shared_ptr<const ContentPack> pack);
void unmount_content_pack(const std::filesystem::path& mount_point);
void unmount_all_content_packs();

// The pack that answers for `path`, or null if it is read from disk.
std::shared_ptr<const ContentPack> content_pack_for(const std::filesystem::path& path);

// A content file's bytes, from a mounted pack or from disk; nullopt if there is
// no such file.
std::optional<std::string> read_content_file(const std::filesystem::path& path);
// The same, for text: Windows line ends ("\r\n") become '\n', as a text-mode
// stream reads them.
std::optional<std::string> read_content_text(const std::filesystem::path& path);
// A content file's bytes without a copy when it is in a pack: the view stays
// valid while the returned owner lives.
struct ContentBytes {
    std::span<const std::byte> bytes;
    std::shared_ptr<const void> owner;
};
std::optional<ContentBytes> view_content_file(const std::filesystem::path& path);
bool content_file_exists(const std::filesystem::path& path);
// True for a folder on disk, and for a folder inside a mounted pack (a mount
// point, or a prefix of one of its files).
bool content_directory_exists(const std::filesystem::path& path);
std::optional<u64> content_file_size(const std::filesystem::path& path);
// The files in `dir` (and its subfolders when `recursive`), as `dir / name`,
// sorted by path.
std::vector<std::filesystem::path> list_content_files(const std::filesystem::path& dir, bool recursive);

// Where find_content_root looks for a game's content.
struct ContentSearch {
    // The content's name beside the executable: `<name>.kinpak` or `<name>/`.
    std::string name = "content";
    // The source content folder, read in place in development builds so edits
    // hot-reload. Empty in shipping builds.
    std::filesystem::path dev_dir;
};

// kin_add_game sets KIN_GAME_CONTENT_NAME (and, outside shipping builds,
// KIN_GAME_CONTENT_DIR) on the game's target; KIN_GAME_CONTENT is the search
// they describe.
#if defined(KIN_GAME_CONTENT_NAME) && defined(KIN_GAME_CONTENT_DIR)
#define KIN_GAME_CONTENT (::kin::ContentSearch{.name = KIN_GAME_CONTENT_NAME, .dev_dir = KIN_GAME_CONTENT_DIR})
#elif defined(KIN_GAME_CONTENT_NAME)
#define KIN_GAME_CONTENT (::kin::ContentSearch{.name = KIN_GAME_CONTENT_NAME, .dev_dir = {}})
#endif

// Finds the game's content and returns the folder to read it from, mounting a
// pack there when the content is one. The first that exists wins:
//   1. `--content=<folder or .kinpak>` in `args`, then the KIN_CONTENT variable;
//   2. `search.dev_dir`;
//   3. `<executable's folder>/<name>.kinpak`;
//   4. `<executable's folder>/<name>/`.
// A pack at `x/name.kinpak` is mounted at `x/name`. Returns an empty path,
// with an error logged, if none exists. The answer is kept for content_root().
std::filesystem::path find_content_root(const ContentSearch& search, int argc = 0, char** argv = nullptr);
// The folder find_content_root last returned (empty before it is called).
std::filesystem::path content_root();
// The folder the running executable is in.
std::filesystem::path executable_dir();

} // namespace kin
