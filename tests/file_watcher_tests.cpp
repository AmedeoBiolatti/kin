#include <kin/assets/file_watcher.hpp>
#include <kin/platform/log.hpp>

#include <cassert>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

// Writes `text` and moves the file's time forward, so a change is seen however
// coarse the file system's clock is.
void write(const fs::path& path, const std::string& text) {
    static fs::file_time_type next = fs::file_time_type::clock::now();
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << text;
    }
    next += std::chrono::seconds(2);
    fs::last_write_time(path, next);
}

bool logged(const std::vector<kin::LogEvent>& events, std::string_view message) {
    for (const kin::LogEvent& event : events) {
        if (event.message == message) {
            return true;
        }
    }
    return false;
}

void test_reports_a_change_once_it_settles(const fs::path& dir) {
    const fs::path path = dir / "settle.txt";
    write(path, "a");
    kin::FileWatcher files;
    std::vector<fs::path> changes;
    files.watch(path, [&](const fs::path& changed) { changes.push_back(changed); });
    assert(files.poll_now() == 0 && changes.empty());

    write(path, "bb");
    assert(files.poll_now() == 0); // seen once: maybe still being written
    assert(files.poll_now() == 1);
    assert(changes.size() == 1 && changes[0] == path);
    assert(files.poll_now() == 0);

    // A file still changing between polls is reported once, after the last write.
    write(path, "ccc");
    assert(files.poll_now() == 0);
    write(path, "dddd");
    assert(files.poll_now() == 0);
    assert(files.poll_now() == 1);
    assert(changes.size() == 2);
}

void test_a_removed_file_is_reported_when_it_returns(const fs::path& dir) {
    const fs::path path = dir / "removed.txt";
    write(path, "a");
    kin::FileWatcher files;
    int changes = 0;
    files.watch(path, [&](const fs::path&) { ++changes; });
    fs::remove(path); // e.g. an editor that saves by replacing the file
    assert(files.poll_now() == 0 && files.poll_now() == 0 && files.poll_now() == 0);
    write(path, "back");
    files.poll_now();
    assert(files.poll_now() == 1 && changes == 1);

    // Watching a file that does not exist yet reports it once it is created.
    const fs::path later = dir / "later.txt";
    fs::remove(later);
    int created = 0;
    files.watch(later, [&](const fs::path&) { ++created; });
    assert(files.poll_now() == 0);
    write(later, "hello");
    files.poll_now();
    files.poll_now();
    assert(created == 1);
}

void test_load_and_watch_keeps_the_last_good_data(const fs::path& dir, const std::vector<kin::LogEvent>& events) {
    const fs::path path = dir / "number.txt";
    write(path, "1");
    int value = 0;
    int loads = 0;
    const kin::FileWatcher::Loader parse = [&](std::string_view text, std::vector<std::string>& errors) {
        ++loads;
        int parsed = 0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
        if (error != std::errc{} || end != text.data() + text.size()) {
            errors.push_back("not a number: " + std::string(text));
            return false; // apply nothing
        }
        value = parsed;
        return true;
    };
    kin::FileWatcher files;
    std::vector<std::string> errors;
    assert(files.load_and_watch(path, parse, &errors));
    assert(value == 1 && errors.empty() && loads == 1);

    write(path, "oops");
    files.poll_now();
    assert(files.poll_now() == 1);
    assert(loads == 2 && value == 1); // rejected: the game keeps running on the last good data
    assert(logged(events, "data file change rejected; keeping the previous data"));

    write(path, "3");
    files.poll_now();
    files.poll_now();
    assert(value == 3);
    assert(logged(events, "data file reloaded"));

    // A missing file fails its first load but is still watched.
    const fs::path missing = dir / "missing.txt";
    fs::remove(missing);
    std::vector<std::string> missing_errors;
    int missing_value = -1;
    assert(!files.load_and_watch(missing, [&](std::string_view text, std::vector<std::string>&) {
        missing_value = static_cast<int>(text.size());
        return true;
    }, &missing_errors));
    assert(missing_errors.size() == 1 && missing_value == -1);
    write(missing, "four");
    files.poll_now();
    files.poll_now();
    assert(missing_value == 4);
}

void test_poll_waits_for_its_interval(const fs::path& dir) {
    const fs::path path = dir / "interval.txt";
    write(path, "a");
    kin::FileWatcher files{{.interval = std::chrono::hours(1)}};
    int changes = 0;
    files.watch(path, [&](const fs::path&) { ++changes; });
    assert(files.poll() == 0); // the first poll checks
    write(path, "b");
    assert(files.poll() == 0 && files.poll() == 0); // too soon: nothing is checked
    files.poll_now();
    assert(files.poll_now() == 1 && changes == 1);
}

void test_callbacks_may_change_the_watch_list(const fs::path& dir) {
    const fs::path path = dir / "self.txt";
    const fs::path other = dir / "other.txt";
    write(path, "a");
    write(other, "a");
    kin::FileWatcher files;
    int changes = 0;
    kin::u32 id = 0;
    id = files.watch(path, [&](const fs::path&) {
        ++changes;
        files.unwatch(id);                    // stop watching itself
        files.watch(other, [](const fs::path&) {}); // and start another
    });
    write(path, "b");
    files.poll_now();
    assert(files.poll_now() == 1 && changes == 1);
    assert(files.size() == 1);
    write(path, "c");
    files.poll_now();
    files.poll_now();
    assert(changes == 1);
}

} // namespace

int main() {
    std::vector<kin::LogEvent> log_events;
    kin::set_logger_config({
        .min_level = kin::LogLevel::Debug,
        .format = kin::LogFormat::Text,
        .sdl_sink = false,
        .memory_events = &log_events,
    });
    const fs::path dir = fs::temp_directory_path() / "kin-file-watcher-tests";
    fs::remove_all(dir);
    fs::create_directories(dir);

    test_reports_a_change_once_it_settles(dir);
    test_a_removed_file_is_reported_when_it_returns(dir);
    test_load_and_watch_keeps_the_last_good_data(dir, log_events);
    test_poll_waits_for_its_interval(dir);
    test_callbacks_may_change_the_watch_list(dir);

    fs::remove_all(dir);
    return 0;
}
