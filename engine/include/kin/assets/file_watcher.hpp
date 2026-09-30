#pragma once

#include <kin/core/types.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kin {

// Watches files and reports the ones that changed, so a game can reload its own
// data (text, balance, themes, levels) while it runs:
//
//   kin::FileWatcher files;
//   files.load_and_watch(root / "balance.kinbalance", [&](std::string_view text, std::vector<std::string>& errors) {
//       return balance.load(text, errors); // apply only on success
//   });
//   config.file_watcher = &files;          // run_scene_app polls it every frame
//
// It polls modification times and sizes, at most every `interval`, and reports
// a change only once the file has stayed the same for two polls: an editor that
// writes in several steps, or saves to a temporary file and renames it, is seen
// once, after it finishes. A file that disappears is not a change; it is
// reported when it comes back. Callbacks run inside poll(), on its thread.
class FileWatcher {
public:
    using Callback = std::function<void(const std::filesystem::path&)>;
    // Parses a whole file; returns false, with messages in `errors`, to reject it.
    using Loader = std::function<bool(std::string_view text, std::vector<std::string>& errors)>;

    struct Options {
        std::chrono::milliseconds interval{250};
    };

    FileWatcher();
    explicit FileWatcher(Options options);

    // Calls `on_change` whenever `path` changes. Returns an id for unwatch().
    u32 watch(std::filesystem::path path, Callback on_change);
    void unwatch(u32 id);

    // Loads `path` with `load` now, then again each time it changes. A file that
    // cannot be read or is rejected is logged, and `load` should then leave the
    // data it had in place, so a mistake in an edit keeps the game running on
    // the last good version. Returns whether the first load succeeded; its
    // messages go to `errors` when given. The file is watched either way.
    bool load_and_watch(std::filesystem::path path, Loader load, std::vector<std::string>* errors = nullptr);

    // Checks the files if `interval` has passed since the last check and runs
    // the callbacks of those that changed. Returns how many ran.
    std::size_t poll();
    // The same, ignoring the interval.
    std::size_t poll_now();

    std::size_t size() const { return _watches.size(); }
    bool empty() const { return _watches.empty(); }

private:
    struct Stamp {
        bool exists = false;
        std::filesystem::file_time_type time{};
        std::uintmax_t bytes = 0;
        bool operator==(const Stamp&) const = default;
    };
    struct Watch {
        u32 id = 0;
        std::filesystem::path path;
        Callback on_change;
        Stamp seen;                   // the version last reported (or found at watch())
        std::optional<Stamp> pending; // a new version, reported once it is seen twice
    };

    static Stamp stamp(const std::filesystem::path& path);

    Options _options;
    std::vector<Watch> _watches;
    u32 _next_id = 1;
    std::chrono::steady_clock::time_point _last_poll{};
    bool _polled = false;
};

// Reads a whole file as text; nullopt if it cannot be read.
std::optional<std::string> read_text_file(const std::filesystem::path& path);

} // namespace kin
