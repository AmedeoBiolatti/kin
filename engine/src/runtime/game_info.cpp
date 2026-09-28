#include <kin/runtime/game_info.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <ostream>
#include <sstream>
#include <stdexcept>

namespace kin {
namespace {

std::string trim(std::string_view text) {
    const auto first = std::ranges::find_if_not(text, [](unsigned char c) {
        return std::isspace(c) != 0;
    });
    const auto last = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char c) {
        return std::isspace(c) != 0;
    }).base();
    if (first >= last) {
        return {};
    }
    return std::string(first, last);
}

std::string trim_comment(std::string line) {
    if (const auto comment = line.find('#'); comment != std::string::npos) {
        line.erase(comment);
    }
    return line;
}

bool parse_bool(std::string_view text, bool& value) {
    if (text == "1" || text == "true" || text == "yes" || text == "on") {
        value = true;
        return true;
    }
    if (text == "0" || text == "false" || text == "no" || text == "off") {
        value = false;
        return true;
    }
    return false;
}

void write_bool(std::ostream& out, bool value) {
    out << (value ? 1 : 0);
}

} // namespace

WindowedAppConfig window_config(const GameInfo& info) {
    return {
        .title = info.title,
        .width = info.window.width,
        .height = info.window.height,
        .logical_width = info.window.logical_width,
        .logical_height = info.window.logical_height,
        .integer_scale = info.window.integer_scale,
        .resizable = info.window.resizable,
        .borderless = info.window.borderless,
        .input_map = info.input_map,
    };
}

void set_field(GameInfo& info, std::string_view key, std::string_view value) {
    for (GameInfoField& field : info.fields) {
        if (field.key == key) {
            field.value = value;
            return;
        }
    }

    info.fields.push_back({
        .key = std::string(key),
        .value = std::string(value),
    });
}

const std::string* field(const GameInfo& info, std::string_view key) {
    for (const GameInfoField& field : info.fields) {
        if (field.key == key) {
            return &field.value;
        }
    }
    return nullptr;
}

GameInfo load_game_info(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) {
        throw std::runtime_error("Failed to open game info: " + path.string());
    }

    GameInfo info;
    std::string line;
    i32 line_no = 0;
    const auto fail = [&](std::string_view message) {
        throw std::runtime_error(path.string() + ":" + std::to_string(line_no) + ": " + std::string{message});
    };

    while (std::getline(file, line)) {
        ++line_no;
        std::istringstream in(trim_comment(std::move(line)));
        std::string kind;
        if (!(in >> kind)) {
            continue;
        }

        const auto read_rest = [&]() {
            std::string rest;
            std::getline(in, rest);
            return trim(rest);
        };

        if (kind == "id") {
            info.id = read_rest();
            continue;
        }
        if (kind == "title") {
            info.title = read_rest();
            continue;
        }
        if (kind == "version") {
            info.version = read_rest();
            continue;
        }
        if (kind == "description") {
            info.description = read_rest();
            continue;
        }
        if (kind == "author") {
            info.author = read_rest();
            continue;
        }
        if (kind == "headless") {
            const std::string value = read_rest();
            if (!parse_bool(value, info.headless_supported)) {
                fail("headless requires a boolean");
            }
            continue;
        }
        if (kind == "window") {
            i32 integer_scale = 0;
            i32 resizable = 0;
            if (!(in >> info.window.width >> info.window.height
                     >> info.window.logical_width >> info.window.logical_height
                     >> integer_scale >> resizable)) {
                fail("window requires width height logical_width logical_height integer_scale resizable [borderless]");
            }
            info.window.integer_scale = integer_scale != 0;
            info.window.resizable = resizable != 0;
            std::string borderless;
            if (in >> borderless) {
                if (!parse_bool(borderless, info.window.borderless)) {
                    fail("window borderless requires a boolean");
                }
            }
            continue;
        }
        if (kind == "tag") {
            const std::string value = read_rest();
            if (value.empty()) {
                fail("tag requires a value");
            }
            info.tags.push_back(value);
            continue;
        }
        if (kind == "field") {
            std::string key;
            if (!(in >> key)) {
                fail("field requires key and value");
            }
            const std::string value = read_rest();
            set_field(info, key, value);
            continue;
        }

        fail("unknown directive");
    }

    return info;
}

bool save_game_info(const GameInfo& info, const std::filesystem::path& path) {
    std::ofstream out(path);
    if (!out) {
        return false;
    }

    out << "# kin game info\n";
    if (!info.id.empty()) {
        out << "id " << info.id << '\n';
    }
    out << "title " << info.title << '\n';
    if (!info.version.empty()) {
        out << "version " << info.version << '\n';
    }
    if (!info.description.empty()) {
        out << "description " << info.description << '\n';
    }
    if (!info.author.empty()) {
        out << "author " << info.author << '\n';
    }
    out << "window "
        << info.window.width << ' '
        << info.window.height << ' '
        << info.window.logical_width << ' '
        << info.window.logical_height << ' ';
    write_bool(out, info.window.integer_scale);
    out << ' ';
    write_bool(out, info.window.resizable);
    out << ' ';
    write_bool(out, info.window.borderless);
    out << '\n';
    out << "headless ";
    write_bool(out, info.headless_supported);
    out << '\n';
    for (const std::string& tag : info.tags) {
        out << "tag " << tag << '\n';
    }
    for (const GameInfoField& field : info.fields) {
        out << "field " << field.key << ' ' << field.value << '\n';
    }

    return static_cast<bool>(out);
}

void write_game_info(std::ostream& out, const GameInfo& info) {
    out << "game\n";
    if (!info.id.empty()) {
        out << "- id: " << info.id << '\n';
    }
    out << "- title: " << info.title << '\n';
    if (!info.version.empty()) {
        out << "- version: " << info.version << '\n';
    }
    if (!info.description.empty()) {
        out << "- description: " << info.description << '\n';
    }
    if (!info.author.empty()) {
        out << "- author: " << info.author << '\n';
    }
    out << "- window: "
        << info.window.width << "x" << info.window.height;
    if (info.window.logical_width > 0 && info.window.logical_height > 0) {
        out << " logical=" << info.window.logical_width << "x" << info.window.logical_height;
    }
    if (info.window.integer_scale) {
        out << " integer-scale";
    }
    if (info.window.resizable) {
        out << " resizable";
    }
    if (info.window.borderless) {
        out << " borderless";
    }
    out << '\n';
    out << "- headless: " << (info.headless_supported ? "yes" : "no") << '\n';

    if (!info.tags.empty()) {
        out << "- tags: ";
        for (std::size_t i = 0; i < info.tags.size(); ++i) {
            if (i > 0) {
                out << ", ";
            }
            out << info.tags[i];
        }
        out << '\n';
    }

    for (const GameInfoField& field : info.fields) {
        out << "- " << field.key << ": " << field.value << '\n';
    }
}

} // namespace kin
