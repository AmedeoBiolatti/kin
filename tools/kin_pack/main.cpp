// kin_pack: a game's content folder as one .kinpak archive, and back.
//
//   kin_pack create <folder> <pack> [--force]   pack a folder (skipped when the pack is up to date)
//   kin_pack list <pack>                        the files in a pack, with their sizes
//   kin_pack verify <pack>                      check every file against its checksum
//   kin_pack extract <pack> <folder>            unpack into a folder
//
// Exit codes: 0 done, 1 failed, 2 bad arguments.

#include <kin/assets/content.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;

std::string utf8(const fs::path& path) {
    const std::u8string text = path.generic_u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

fs::path from_utf8(std::string_view text) {
    return fs::path{std::u8string{reinterpret_cast<const char8_t*>(text.data()), text.size()}};
}

// A command-line argument as a path: argv is in the system's encoding, which
// is how std::filesystem takes a narrow string.
fs::path arg_path(std::string_view arg) {
    return fs::path{std::string{arg}};
}

int usage() {
    std::cerr << "usage: kin_pack create <folder> <pack> [--force]\n"
                 "       kin_pack list <pack>\n"
                 "       kin_pack verify <pack>\n"
                 "       kin_pack extract <pack> <folder>\n";
    return 2;
}

std::shared_ptr<const kin::ContentPack> open_pack(const fs::path& file) {
    std::string error;
    std::shared_ptr<const kin::ContentPack> pack = kin::ContentPack::open(file, &error);
    if (!pack) {
        std::cerr << "kin_pack: " << error << '\n';
    }
    return pack;
}

// The pack holds exactly the folder's files (as write_content_pack picks
// them), at their sizes, and was written after the newest of them.
bool up_to_date(const fs::path& dir, const fs::path& out) {
    std::error_code error;
    if (!fs::is_regular_file(out, error)) {
        return false;
    }
    const std::shared_ptr<const kin::ContentPack> pack = kin::ContentPack::open(out);
    if (!pack) {
        return false;
    }
    const fs::file_time_type packed = fs::last_write_time(out, error);
    if (error) {
        return false;
    }
    std::vector<std::pair<std::string, std::uintmax_t>> files;
    for (auto it = fs::recursive_directory_iterator(dir, error); !error && it != fs::recursive_directory_iterator();
         it.increment(error)) {
        if (utf8(it->path().filename()).starts_with('.')) {
            if (it->is_directory(error)) {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (!it->is_regular_file(error)) {
            continue;
        }
        if (it->last_write_time(error) > packed) {
            return false;
        }
        files.emplace_back(utf8(it->path().lexically_relative(dir)), it->file_size(error));
    }
    if (error || files.size() != pack->entries().size()) {
        return false;
    }
    std::ranges::sort(files);
    for (std::size_t i = 0; i < files.size(); ++i) {
        const kin::ContentPack::Entry& entry = pack->entries()[i];
        if (entry.path != files[i].first || entry.size != files[i].second) {
            return false;
        }
    }
    return true;
}

int create(const fs::path& dir, const fs::path& out, bool force) {
    if (!force && up_to_date(dir, out)) {
        std::cout << "kin_pack: " << out.string() << " is up to date\n";
        return 0;
    }
    const kin::ContentPackWriteResult result = kin::write_content_pack(dir, out);
    if (!result.ok) {
        std::cerr << "kin_pack: " << result.error << '\n';
        return 1;
    }
    std::cout << "kin_pack: " << out.string() << ": " << result.files << " files, " << result.bytes << " bytes\n";
    return 0;
}

int list(const fs::path& file) {
    const std::shared_ptr<const kin::ContentPack> pack = open_pack(file);
    if (!pack) {
        return 1;
    }
    for (const kin::ContentPack::Entry& entry : pack->entries()) {
        std::cout << entry.size << '\t' << entry.path << '\n';
    }
    return 0;
}

int verify(const fs::path& file) {
    const std::shared_ptr<const kin::ContentPack> pack = open_pack(file);
    if (!pack) {
        return 1;
    }
    std::string error;
    if (!pack->verify(&error)) {
        std::cerr << "kin_pack: " << file.string() << ": " << error << '\n';
        return 1;
    }
    std::cout << "kin_pack: " << file.string() << ": " << pack->entries().size() << " files check out\n";
    return 0;
}

int extract(const fs::path& file, const fs::path& dir) {
    const std::shared_ptr<const kin::ContentPack> pack = open_pack(file);
    if (!pack) {
        return 1;
    }
    for (const kin::ContentPack::Entry& entry : pack->entries()) {
        const fs::path target = dir / from_utf8(entry.path); // paths were checked on open: none leave `dir`
        std::error_code error;
        fs::create_directories(target.parent_path(), error);
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        const std::span<const std::byte> bytes = pack->bytes(entry);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out) {
            std::cerr << "kin_pack: cannot write " << target.string() << '\n';
            return 1;
        }
    }
    std::cout << "kin_pack: " << pack->entries().size() << " files into " << dir.string() << '\n';
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string_view> args(argv + 1, argv + argc);
    const bool force = std::erase(args, std::string_view{"--force"}) > 0;
    if (args.empty()) {
        return usage();
    }
    const std::string_view command = args[0];
    if (command == "create" && args.size() == 3) {
        return create(arg_path(args[1]), arg_path(args[2]), force);
    }
    if (command == "list" && args.size() == 2) {
        return list(arg_path(args[1]));
    }
    if (command == "verify" && args.size() == 2) {
        return verify(arg_path(args[1]));
    }
    if (command == "extract" && args.size() == 3) {
        return extract(arg_path(args[1]), arg_path(args[2]));
    }
    return usage();
}
