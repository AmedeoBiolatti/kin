#include <kin/assets/content.hpp>

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

using namespace kin;

namespace {

void test_reads_serve_the_snapshot() {
    const auto content = MemoryContent::create({{"a.txt", "one"}, {"dir/b.txt", "two"}, {"dir/deep/c.txt", "three"}});
    const std::filesystem::path root = content->root();
    assert(read_content_file(root / "a.txt") == "one");
    assert(read_content_text(root / "dir/b.txt") == "two");
    assert(content_file_exists(root / "dir/deep/c.txt") && !content_file_exists(root / "missing.txt"));
    assert(!content_file_exists(root) && content_directory_exists(root));
    assert(content_directory_exists(root / "dir") && !content_directory_exists(root / "di"));
    assert(content_file_size(root / "dir/deep/c.txt") == 5u);
    assert(list_content_files(root / "dir", false) == std::vector<std::filesystem::path>{root / "dir/b.txt"});
    assert(list_content_files(root, true).size() == 3);
    assert(memory_content_for(root / "a.txt") == content && !memory_content_for("elsewhere/a.txt"));
    assert(!content_pack_for("elsewhere/a.txt"));
}

// Listed names are UTF-8 in the snapshot and paths on every platform.
void test_listing_keeps_non_ascii_names() {
    const std::string name = "caf\xC3\xA9.txt";
    const auto content = MemoryContent::create({{"dir/" + name, "x"}});
    const auto files = list_content_files(content->root() / "dir", false);
    assert(files.size() == 1);
    const std::u8string expected{reinterpret_cast<const char8_t*>(name.data()), name.size()};
    assert(files[0].filename() == std::filesystem::path{expected});
    assert(read_content_file(files[0]) == "x");
}

// A snapshot is mounted while it lives; views keep their bytes after it is gone.
void test_lifetime() {
    auto content = MemoryContent::create({{"a.txt", "alive"}});
    const std::filesystem::path root = content->root();
    auto view = view_content_file(root / "a.txt");
    assert(view && view->bytes.size() == 5);
    unmount_all_content_packs(); // packs mounted by a game, not snapshots
    assert(read_content_file(root / "a.txt") == "alive");
    content.reset();
    assert(!read_content_file(root / "a.txt") && !memory_content_for(root / "a.txt"));
    assert(std::string(reinterpret_cast<const char*>(view->bytes.data()), view->bytes.size()) == "alive");
}

bool rejects(MemoryContent::Files files) {
    try {
        MemoryContent::create(std::move(files));
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

void test_invalid_paths() {
    assert(rejects({{"../escape.txt", ""}}) && rejects({{"/absolute.txt", ""}}) && rejects({{"a\\b.txt", ""}}));
}

// Code that resolves paths (scripts do) reaches the files through the temp
// folder's canonical name too: Windows may give it as RUNNER~1, POSIX through a link.
void test_canonical_root() {
#ifndef _WIN32 // links need privileges there
    namespace fs = std::filesystem;
    const fs::path temp = fs::temp_directory_path();
    const fs::path real = temp / "kin-memory-content-real";
    const fs::path link = temp / "kin-memory-content-link";
    fs::remove_all(real);
    fs::remove(link);
    fs::create_directories(real);
    fs::create_directory_symlink(real, link);
    const char* previous = std::getenv("TMPDIR");
    const std::string kept = previous ? previous : "";
    setenv("TMPDIR", link.c_str(), 1);
    const auto content = MemoryContent::create({{"a.txt", "linked"}});
    previous ? setenv("TMPDIR", kept.c_str(), 1) : unsetenv("TMPDIR");
    assert(content->root().string().starts_with(fs::absolute(link).lexically_normal().string()));
    const fs::path canonical = fs::weakly_canonical(content->root());
    assert(canonical != content->root() && read_content_file(canonical / "a.txt") == "linked");
    fs::remove(link);
    fs::remove_all(real);
#endif
}

} // namespace

int main() {
    test_reads_serve_the_snapshot();
    test_listing_keeps_non_ascii_names();
    test_lifetime();
    test_invalid_paths();
    test_canonical_root();
}
