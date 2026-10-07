#include <kin/assets/asset_manager.hpp>
#include <kin/assets/content.hpp>
#include <kin/assets/file_watcher.hpp>
#include <kin/assets/image.hpp>
#include <kin/audio/audio_clip.hpp>
#include <kin/audio/backend.hpp>
#include <kin/l10n/localization.hpp>
#include <kin/scripting/lua_script.hpp>
#include <kin/scripting/script_engine.hpp>
#include <kin/ui2/text.hpp>

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

void write(const fs::path& path, std::string_view bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// A 2x2 BMP, top-down BGRA.
std::string bmp() {
    const unsigned char bytes[] = {
        0x42, 0x4d, 0x46, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x36, 0x00, 0x00, 0x00, 0x28, 0x00,
        0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0xfe, 0xff, 0xff, 0xff, 0x01, 0x00, 0x20, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0x00, 0xff, 0x00, 0xff, 0xff, 0x00,
        0x00, 0xff, 0xff, 0xff, 0xff, 0xff,
    };
    return {reinterpret_cast<const char*>(bytes), sizeof(bytes)};
}

// A WAV of four silent 16-bit mono frames at 8 kHz.
std::string wav() {
    std::string out;
    const auto u32 = [&](unsigned v) { for (int i = 0; i < 4; ++i) out += static_cast<char>((v >> (8 * i)) & 0xFF); };
    const auto u16 = [&](unsigned v) { for (int i = 0; i < 2; ++i) out += static_cast<char>((v >> (8 * i)) & 0xFF); };
    out += "RIFF"; u32(36 + 8); out += "WAVE";
    out += "fmt "; u32(16); u16(1); u16(1); u32(8000); u32(16000); u16(2); u16(16);
    out += "data"; u32(8); out += std::string(8, '\0');
    return out;
}

// A font this machine has, to pack; empty if none.
fs::path some_font() {
    for (const char* candidate : {"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                                  "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf",
                                  "C:/Windows/Fonts/arial.ttf", "C:/Windows/Fonts/segoeui.ttf"}) {
        std::error_code error;
        if (fs::is_regular_file(candidate, error)) {
            return candidate;
        }
    }
    return {};
}

fs::path make_content(const fs::path& dir, const fs::path& font) {
    fs::remove_all(dir);
    write(dir / "hero.bmp", bmp());
    write(dir / "sounds/hit.wav", wav());
    write(dir / "data/rules.txt", "first\r\nsecond\r\n");
    write(dir / "lang/en.kinlang", R"({"locale": "en", "name": "English", "strings": {"menu": {"play": "Play"}}})");
    write(dir / "lang/fr.kinlang", R"({"locale": "fr", "name": "Français", "strings": {"menu": {"play": "Jouer"}}})");
    write(dir / "l10n/fr/hero.bmp", bmp());
    write(dir / "scripts/main.lua", "local util = require('util')\nfunction answer() return util.double(21) end\n");
    write(dir / "scripts/util.lua", "return { double = function(x) return x * 2 end }\n");
    write(dir / "scripts/broken.lua", "function oops(\n");
    write(dir / ".hidden", "left out");
    write(dir / ".git/config", "left out");
    if (!font.empty()) {
        fs::create_directories(dir / "fonts");
        fs::copy_file(font, dir / "fonts/ui.ttf", fs::copy_options::overwrite_existing);
    }
    return dir;
}

void test_pack_round_trip(const fs::path& work, const fs::path& source) {
    const fs::path out = work / "game.kinpak";
    const kin::ContentPackWriteResult written = kin::write_content_pack(source, out);
    assert(written.ok && written.error.empty());
    assert(!fs::exists(work / "game.kinpak.partial"));

    std::string error;
    const auto pack = kin::ContentPack::open(out, &error);
    assert(pack && error.empty());
    assert(pack->entries().size() == written.files);
    // In path order, without dotfiles or dot folders.
    std::vector<std::string> paths;
    for (const auto& entry : pack->entries()) {
        paths.push_back(entry.path);
        assert(entry.offset % 16 == 0);
        assert(!entry.path.starts_with('.') && entry.path.find("/.") == std::string::npos);
    }
    assert(std::ranges::is_sorted(paths));
    assert(std::ranges::find(paths, "data/rules.txt") != paths.end());
    // Bytes as they were on disk, and the checksums hold.
    const auto* rules = pack->find("data/rules.txt");
    assert(rules && rules->size == 15);
    const auto bytes = pack->bytes(*rules);
    assert(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()) == "first\r\nsecond\r\n");
    assert(!pack->find("data/Rules.txt") && pack->find_ignoring_case("data/Rules.txt") == rules);
    assert(!pack->find("data") && !pack->find("rules.txt"));
    assert(pack->verify(&error));

    // Writing again replaces the pack in place.
    assert(kin::write_content_pack(source, out).ok);
    assert(kin::write_content_pack(work / "no-such-folder", work / "other.kinpak").error.size() > 0);
}

void test_damaged_packs(const fs::path& work) {
    const std::string good = read(work / "game.kinpak");
    const auto refuses = [&](std::string bytes, std::string_view why) {
        const fs::path path = work / "damaged.kinpak";
        write(path, bytes);
        std::string error;
        assert(!kin::ContentPack::open(path, &error));
        assert(error.find(why) != std::string::npos);
    };
    refuses("short", "too short");
    refuses(std::string(64, 'x'), "not a kin pack");
    std::string version = good;
    version[8] = 9;
    refuses(version, "version 9");
    std::string index = good;
    index[index.size() - 2] ^= 0x20; // a path's letter, in the index
    refuses(index, "index checksum");
    std::string truncated = good.substr(0, good.size() - 10);
    refuses(truncated, "index out of range");
    std::string error;
    assert(!kin::ContentPack::open(work / "missing.kinpak", &error) && !error.empty());

    // A file's bytes changed: the index still reads, verify() finds it.
    const auto pack = kin::ContentPack::open(work / "game.kinpak");
    const auto* rules = pack->find("data/rules.txt");
    std::string data = good;
    data[static_cast<std::size_t>(rules->offset)] = 'F';
    write(work / "damaged.kinpak", data);
    const auto damaged = kin::ContentPack::open(work / "damaged.kinpak");
    assert(damaged);
    assert(!damaged->verify(&error) && error.find("data/rules.txt") != std::string::npos);
}

// The same reads, from the folder on disk and from the pack mounted somewhere
// that does not exist.
void test_reads(const fs::path& root) {
    assert(kin::content_file_exists(root / "hero.bmp"));
    assert(!kin::content_file_exists(root / "nope.bmp"));
    assert(!kin::content_file_exists(root / "data"));
    assert(kin::content_directory_exists(root));
    assert(kin::content_directory_exists(root / "data"));
    assert(kin::content_directory_exists(root / "data/"));
    assert(!kin::content_directory_exists(root / "dat"));
    assert(!kin::content_directory_exists(root / "hero.bmp"));
    assert(kin::content_file_size(root / "data/rules.txt") == 15u);
    assert(!kin::content_file_size(root / "data"));
    assert(kin::read_content_file(root / "data/rules.txt") == "first\r\nsecond\r\n");
    assert(kin::read_content_text(root / "data/rules.txt") == "first\nsecond\n");
    assert(kin::read_text_file(root / "data/rules.txt") == "first\r\nsecond\r\n");
    assert(!kin::read_content_file(root / "missing.txt"));
    assert(!kin::read_content_file(root / "data/RULES.txt") || !kin::content_pack_for(root)); // case counts in packs
    assert(kin::read_content_file(root / "scripts/../data/rules.txt"));
    const auto view = kin::view_content_file(root / "data/rules.txt");
    assert(view && view->owner && view->bytes.size() == 15);

    const auto lang = kin::list_content_files(root / "lang", false);
    assert(lang.size() == 2 && lang[0] == root / "lang/en.kinlang" && lang[1] == root / "lang/fr.kinlang");
    const auto top = kin::list_content_files(root, false);
    assert(std::ranges::find(top, root / "hero.bmp") != top.end());
    assert(std::ranges::find(top, root / "data/rules.txt") == top.end());
    const auto all = kin::list_content_files(root, true);
    assert(std::ranges::find(all, root / "data/rules.txt") != all.end());
    assert(std::ranges::is_sorted(all));
    assert(kin::list_content_files(root / "nothing", true).empty());

    // Loaders: an image (SDL_image), a sound (SDL), language files, scripts.
    const kin::Image hero = kin::load_image(root / "hero.bmp");
    assert(hero.size.x == 2 && hero.size.y == 2);
    assert(kin::load_audio_clip(root / "sounds/hit.wav").valid());
    assert(kin::load_audio_stream(root / "sounds/hit.wav").valid()); // streamed music reads the same way

    kin::AssetManager assets{root};
    kin::register_default_asset_loaders(assets);
    assert(assets.load<kin::Image>("hero.bmp")->size.x == 2);
    assets.discover();
    const kin::AssetMetadata* rules = assets.metadata("data/rules.txt");
    assert(rules && rules->size_bytes == 15 && rules->status == kin::AssetStatus::Discovered);

    kin::Localization l10n;
    std::vector<std::string> errors;
    assert(l10n.load_directory(root / "lang", errors) && errors.empty());
    l10n.set_locale("fr");
    assert(l10n.tr("menu.play") == "Jouer");
    // Assets by language: l10n/fr/hero.bmp stands in for hero.bmp.
    assert(l10n.localized_path(root, "hero.bmp") == "l10n/fr/hero.bmp");
    assert(l10n.localized_path(root, "sounds/hit.wav") == "sounds/hit.wav");
    assert(!l10n.load_directory(root / "no-lang", errors));

    kin::LuaScriptOptions options;
    options.module_root = root / "scripts";
    kin::LuaScript script{options};
    assert(script.load_file(root / "scripts/main.lua"));
    assert(script.call_for<int>("answer") == 42);
    kin::LuaScript broken;
    assert(!broken.load_file(root / "scripts/broken.lua"));
    assert(broken.error().find("broken.lua") != std::string::npos);
    assert(!broken.load_file(root / "scripts/missing.lua"));

    kin::ScriptEngine engine;
    assert(engine.load_file(root, "scripts/main.lua"));
    assert(!engine.load_file(root, "scripts/missing.lua"));
    assert(engine.last_error().find("cannot open") != std::string::npos);
    assert(!engine.load_file(root, "scripts/broken.lua"));
    assert(engine.last_error().find("broken.lua") != std::string::npos);

    if (kin::content_file_exists(root / "fonts/ui.ttf")) {
        const kin::ui2::Font font = kin::ui2::load_ttf_font(root / "fonts/ui.ttf", 16.0f);
        assert(kin::ui2::measure_text(font, "Play", 1.0f).x > 0.0f);
    }
}

void test_mounts(const fs::path& work, const fs::path& source) {
    test_reads(source);

    const fs::path root = work / "mounted" / "game"; // not a folder on disk
    assert(!fs::exists(root));
    kin::mount_content_pack(root, kin::ContentPack::open(work / "game.kinpak"));
    assert(kin::content_pack_for(root / "hero.bmp"));
    assert(kin::content_pack_for(root));
    assert(!kin::content_pack_for(work / "mounted"));
    assert(!kin::content_pack_for(work / "mounted" / "gamer" / "hero.bmp"));
    test_reads(root);
    // Relative paths resolve from the working folder.
    const fs::path cwd = fs::current_path();
    fs::current_path(work);
    assert(kin::content_file_exists(fs::path{"mounted/game/hero.bmp"}));
    fs::current_path(cwd);
    // The pack, not the disk, answers under its mount point.
    write(root / "disk-only.txt", "on disk");
    assert(!kin::content_file_exists(root / "disk-only.txt"));
    fs::remove_all(work / "mounted");

    // A file opened from a pack outlives its unmounting.
    const auto view = kin::view_content_file(root / "data/rules.txt");
    kin::unmount_content_pack(root);
    assert(!kin::content_file_exists(root / "hero.bmp"));
    assert(view && std::memcmp(view->bytes.data(), "first", 5) == 0);
}

// A path that reaches the mount point another way (a symlink here; on Windows
// also a short 8.3 name) still reaches the pack once it is made canonical, as
// the Lua loaders make module paths.
void test_mount_through_link(const fs::path& work) {
    std::error_code error;
    fs::create_directories(work / "real", error);
    fs::create_directory_symlink(work / "real", work / "link", error);
    if (error) {
        return; // no symlinks here (Windows without the privilege)
    }
    const fs::path root = work / "link" / "game";
    kin::mount_content_pack(root, kin::ContentPack::open(work / "game.kinpak"));
    const fs::path real = fs::weakly_canonical(work / "real"); // also a Windows short name made long
    assert(kin::content_file_exists(real / "game" / "hero.bmp"));
    assert(kin::content_file_exists(fs::weakly_canonical(root / "scripts" / "util.lua")));
    kin::LuaScriptOptions options;
    options.module_root = root / "scripts";
    kin::LuaScript script{options};
    assert(script.load_file(root / "scripts/main.lua"));
    assert(script.call_for<int>("answer") == 42);
    kin::unmount_content_pack(root);
    assert(!kin::content_file_exists(real / "game" / "hero.bmp"));
}

void test_find_content_root(const fs::path& work, const fs::path& source) {
    // The source folder, while the game is made.
    assert(kin::find_content_root({.name = "game", .dev_dir = source}) == source.lexically_normal());
    assert(kin::content_root() == source.lexically_normal());

    // --content=<pack> mounts it beside itself, under its name.
    std::string arg = "--content=" + (work / "game.kinpak").string();
    char program[] = "game";
    char* argv[] = {program, arg.data()};
    const fs::path root = kin::find_content_root({.name = "game", .dev_dir = source}, 2, argv);
    assert(root == (work / "game").lexically_normal());
    assert(kin::content_pack_for(root) && kin::content_file_exists(root / "hero.bmp"));
    kin::unmount_content_pack(root);

    // --content <folder> as two arguments.
    std::string flag = "--content", folder = source.string();
    char* split[] = {program, flag.data(), folder.data()};
    assert(kin::find_content_root({.name = "game", .dev_dir = {}}, 3, split) == source.lexically_normal());

    // Nothing there: an empty root, the content not found.
    std::string missing = "--content=" + (work / "nowhere").string();
    char* none[] = {program, missing.data()};
    assert(kin::find_content_root({.name = "game", .dev_dir = {}}, 2, none).empty());
    assert(kin::content_root().empty());
    assert(kin::find_content_root({.name = "no-such-game-content", .dev_dir = work / "nowhere"}).empty());
    assert(!kin::executable_dir().empty());
}

} // namespace

int main() try {
    const fs::path work = fs::temp_directory_path() / "kin_content_tests";
    fs::remove_all(work);
    fs::create_directories(work);
    const fs::path source = make_content(work / "source", some_font());

    test_pack_round_trip(work, source);
    test_damaged_packs(work);
    test_mounts(work, source);
    test_mount_through_link(work);
    test_find_content_root(work, source);

    kin::unmount_all_content_packs();
    fs::remove_all(work);
    return 0;
} catch (const std::exception& e) {
    std::cerr << "kin-content: " << e.what() << '\n';
    return 1;
}
