#include <kin/assets/file_watcher.hpp>

#include <kin/assets/content.hpp>
#include <kin/platform/log.hpp>

#include <algorithm>
#include <memory>
#include <utility>

namespace kin {

std::optional<std::string> read_text_file(const std::filesystem::path& path) {
    return read_content_file(path);
}

FileWatcher::FileWatcher()
    : FileWatcher(Options{}) {
}

FileWatcher::FileWatcher(Options options)
    : _options(options) {
}

FileWatcher::Stamp FileWatcher::stamp(const std::filesystem::path& path) {
    std::error_code error;
    Stamp result;
    result.time = std::filesystem::last_write_time(path, error);
    if (error) {
        return {};
    }
    result.bytes = std::filesystem::file_size(path, error);
    if (error) {
        return {};
    }
    result.exists = true;
    return result;
}

u32 FileWatcher::watch(std::filesystem::path path, Callback on_change) {
    const u32 id = _next_id++;
    Stamp seen = stamp(path);
    _watches.push_back({.id = id, .path = std::move(path), .on_change = std::move(on_change), .seen = seen});
    return id;
}

void FileWatcher::unwatch(u32 id) {
    std::erase_if(_watches, [id](const Watch& watch) { return watch.id == id; });
}

namespace {

// Runs `load` over the file's text, logging whatever goes wrong.
bool load_file(const std::filesystem::path& path, const FileWatcher::Loader& load, std::vector<std::string>& errors,
               bool reload) {
    const std::optional<std::string> text = read_text_file(path);
    if (!text) {
        errors.push_back("could not be read");
    } else if (load(*text, errors)) {
        if (reload) {
            KIN_LOG_INFO_F("assets", "data file reloaded", (LogFields{{.name = "path", .value = path.string()}}));
        }
        return true;
    }
    if (errors.empty()) {
        errors.push_back("rejected");
    }
    for (const std::string& error : errors) {
        KIN_LOG_WARN_F("assets", reload ? "data file change rejected; keeping the previous data" : "data file rejected",
                       (LogFields{{.name = "path", .value = path.string()}, {.name = "error", .value = error}}));
    }
    return false;
}

} // namespace

bool FileWatcher::load_and_watch(std::filesystem::path path, Loader load, std::vector<std::string>* errors, u32* id) {
    auto shared = std::make_shared<Loader>(std::move(load));
    std::vector<std::string> first_errors;
    const bool loaded = load_file(path, *shared, first_errors, false);
    if (errors) {
        errors->insert(errors->end(), first_errors.begin(), first_errors.end());
    }
    const u32 watch_id = watch(std::move(path), [shared](const std::filesystem::path& changed) {
        std::vector<std::string> reload_errors;
        load_file(changed, *shared, reload_errors, true);
    });
    if (id) {
        *id = watch_id;
    }
    return loaded;
}

std::size_t FileWatcher::poll() {
    const auto now = std::chrono::steady_clock::now();
    if (_polled && now - _last_poll < _options.interval) {
        return 0;
    }
    return poll_now();
}

std::size_t FileWatcher::poll_now() {
    _last_poll = std::chrono::steady_clock::now();
    _polled = true;

    // Decide first, then call back: a callback may watch or unwatch files.
    std::vector<std::pair<Callback, std::filesystem::path>> changed;
    for (Watch& watch : _watches) {
        const Stamp current = stamp(watch.path);
        if (current == watch.seen) {
            watch.pending.reset();
            continue;
        }
        if (!watch.pending || *watch.pending != current) {
            watch.pending = current; // still settling: confirm on the next poll
            continue;
        }
        watch.seen = current;
        watch.pending.reset();
        if (current.exists) {
            changed.emplace_back(watch.on_change, watch.path);
        }
    }
    for (auto& [callback, path] : changed) {
        callback(path);
    }
    return changed.size();
}

} // namespace kin
