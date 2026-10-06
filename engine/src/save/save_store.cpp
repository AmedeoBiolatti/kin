#include <kin/save/save_store.hpp>

#include <kin/platform/user_data.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <optional>
#include <sstream>
#include <system_error>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace kin {
namespace {

constexpr std::string_view settings_schema = "kin.settings/1";
constexpr std::string_view save_schema = "kin.save/1";

std::optional<i64>& save_time_override() {
    static std::optional<i64> value;
    return value;
}

SaveResult ok_result(std::filesystem::path path = {}) {
    return {.ok = true, .error = SaveErrorCode::None, .path = std::move(path)};
}

SaveResult error_result(SaveErrorCode error, std::string message, std::filesystem::path path = {}) {
    return {.ok = false, .error = error, .message = std::move(message), .path = std::move(path)};
}

SaveLoadResult load_error(SaveErrorCode error, std::string message, std::filesystem::path path = {}) {
    return {.result = error_result(error, std::move(message), std::move(path))};
}

bool has_json_extension(const std::filesystem::path& path) {
    return path.extension() == ".json";
}

bool is_temp_path(const std::filesystem::path& path) {
    const std::string filename = path.filename().string();
    return filename.ends_with(".tmp") || path.extension() == ".tmp";
}

std::string read_text_file(const std::filesystem::path& path, bool& ok) {
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        ok = false;
        return {};
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    ok = in.good() || in.eof();
    return buffer.str();
}

SaveResult atomic_write_text(const std::filesystem::path& path, const std::string& text) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        return error_result(SaveErrorCode::IoError,
                            "failed to create save directory: " + ec.message(),
                            path);
    }

    const std::filesystem::path temp = path.string() + ".tmp";
    {
        std::ofstream out{temp, std::ios::binary | std::ios::trunc};
        if (!out) {
            return error_result(SaveErrorCode::IoError, "failed to open temp save file", temp);
        }
        out << text;
        out.flush();
        if (!out) {
            std::filesystem::remove(temp, ec);
            return error_result(SaveErrorCode::IoError, "failed to write temp save file", temp);
        }
    }

#if defined(_WIN32)
    if (!MoveFileExW(temp.native().c_str(),
                     path.native().c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD failure = GetLastError();
        std::filesystem::remove(temp, ec);
        return error_result(SaveErrorCode::IoError,
                            "failed to replace save file: Windows error " + std::to_string(failure),
                            path);
    }
#else
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        return error_result(SaveErrorCode::IoError,
                            "failed to replace save file: " + ec.message(),
                            path);
    }
#endif
    return ok_result(path);
}

SaveResult validate_envelope(const JsonValue& root,
                             std::string_view expected_schema,
                             std::string_view game_id,
                             const std::filesystem::path& path) {
    if (!root.is_object()) {
        return error_result(SaveErrorCode::SchemaMismatch, "save file root is not an object", path);
    }
    const JsonValue* schema = root.find("schema");
    if (!schema || !schema->is_string()) {
        return error_result(SaveErrorCode::SchemaMismatch, "save file is missing schema", path);
    }
    if (schema->as_string() != expected_schema) {
        return error_result(SaveErrorCode::UnsupportedVersion, "unsupported save schema", path);
    }
    const JsonValue* envelope_game_id = root.find("game_id");
    if (!envelope_game_id || !envelope_game_id->is_string()) {
        return error_result(SaveErrorCode::SchemaMismatch, "save file is missing game_id", path);
    }
    if (envelope_game_id->as_string() != game_id) {
        return error_result(SaveErrorCode::GameMismatch, "save file belongs to a different game", path);
    }
    return ok_result(path);
}

SaveSlotInfo parse_slot_info(const JsonValue& root) {
    SaveSlotInfo info;
    info.slot_id = root.string_at("slot_id");
    info.title = root.string_at("title");
    info.game_version = root.string_at("game_version");
    info.created_at_unix = root.int_at("created_at");
    info.updated_at_unix = root.int_at("updated_at");
    info.play_time_seconds = root.int_at("play_time_seconds");
    info.payload_version = static_cast<i32>(root.int_at("payload_version", 1));
    info.summary = root.string_at("summary");
    return info;
}

void write_payload_field(JsonWriter& json, const std::function<void(JsonWriter&)>& write_payload) {
    json.key("payload");
    if (write_payload) {
        write_payload(json);
    } else {
        json.value_null();
    }
}

} // namespace

void set_save_time_override(i64 unix_seconds) {
    save_time_override() = unix_seconds;
}

void clear_save_time_override() {
    save_time_override().reset();
}

i64 current_save_time_unix() {
    if (save_time_override()) {
        return *save_time_override();
    }
    const auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
}

bool valid_save_id(std::string_view id) {
    if (id.empty() || id == "." || id == "..") {
        return false;
    }
    return std::ranges::all_of(id, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}

SaveStore::SaveStore(SaveStoreConfig config)
    : _game_id(std::move(config.game_id)),
      _game_version(std::move(config.game_version)),
      _root(config.root_override.empty() ? user_data_dir("kin") / _game_id : std::move(config.root_override)),
      _valid_game_id(valid_save_id(_game_id)),
      _compact(config.compact) {
}

std::filesystem::path SaveStore::settings_path() const {
    return _root / "settings.json";
}

std::filesystem::path SaveStore::slot_path(std::string_view slot_id) const {
    return _root / "saves" / (std::string{slot_id} + ".json");
}

SaveResult SaveStore::validate_store() const {
    if (!_valid_game_id) {
        return error_result(SaveErrorCode::InvalidId, "invalid game id", _root);
    }
    return ok_result(_root);
}

SaveResult SaveStore::validate_slot(std::string_view slot_id) const {
    if (SaveResult store = validate_store(); !store.ok) {
        return store;
    }
    if (!valid_save_id(slot_id)) {
        return error_result(SaveErrorCode::InvalidId, "invalid slot id", slot_path(slot_id));
    }
    return ok_result(slot_path(slot_id));
}

SaveResult SaveStore::write_settings(std::function<void(JsonWriter&)> write_payload) {
    if (SaveResult store = validate_store(); !store.ok) {
        return store;
    }
    const i64 now = current_save_time_unix();
    std::ostringstream out;
    JsonWriter json(out, !_compact);
    json.begin_object();
    json.field("schema", settings_schema);
    json.field("game_id", std::string_view{_game_id});
    json.field("game_version", std::string_view{_game_version});
    json.field("updated_at", now);
    write_payload_field(json, write_payload);
    json.end_object();
    out << '\n';
    return atomic_write_text(settings_path(), out.str());
}

SaveLoadResult SaveStore::read_settings() const {
    if (SaveResult store = validate_store(); !store.ok) {
        return {.result = store};
    }
    const std::filesystem::path path = settings_path();
    if (!std::filesystem::exists(path)) {
        return load_error(SaveErrorCode::NotFound, "settings file not found", path);
    }
    bool read_ok = false;
    const std::string text = read_text_file(path, read_ok);
    if (!read_ok) {
        return load_error(SaveErrorCode::IoError, "failed to read settings file", path);
    }
    JsonParseResult parsed = parse_json(text);
    if (!parsed.ok()) {
        return load_error(SaveErrorCode::ParseError, parsed.error, path);
    }
    JsonValue& root = *parsed.value;
    if (SaveResult envelope = validate_envelope(root, settings_schema, _game_id, path); !envelope.ok) {
        return {.result = envelope};
    }

    SaveLoadResult result;
    result.result = ok_result(path);
    result.info.game_version = root.string_at("game_version");
    result.info.updated_at_unix = root.int_at("updated_at");
    if (std::optional<JsonValue> payload = root.take_member("payload")) {
        result.payload = std::move(*payload);
    }
    return result;
}

SaveResult SaveStore::write_slot(std::string_view slot_id,
                                 const SaveSlotInfo& info,
                                 std::function<void(JsonWriter&)> write_payload) {
    if (SaveResult slot = validate_slot(slot_id); !slot.ok) {
        return slot;
    }
    const i64 now = current_save_time_unix();
    const i64 created_at = info.created_at_unix != 0 ? info.created_at_unix : now;
    const i64 updated_at = info.updated_at_unix != 0 ? info.updated_at_unix : now;

    std::ostringstream out;
    JsonWriter json(out, !_compact);
    json.begin_object();
    json.field("schema", save_schema);
    json.field("game_id", std::string_view{_game_id});
    json.field("game_version", std::string_view{_game_version});
    json.field("slot_id", slot_id);
    json.field("title", std::string_view{info.title});
    json.field("created_at", created_at);
    json.field("updated_at", updated_at);
    json.field("play_time_seconds", info.play_time_seconds);
    json.field("payload_version", info.payload_version);
    if (!info.summary.empty()) {
        json.field("summary", std::string_view{info.summary});
    }
    write_payload_field(json, write_payload);
    json.end_object();
    out << '\n';
    return atomic_write_text(slot_path(slot_id), out.str());
}

SaveLoadResult SaveStore::read_slot(std::string_view slot_id) const {
    return read_slot_file(slot_id, true);
}

SaveLoadResult SaveStore::read_slot_file(std::string_view slot_id, bool with_payload) const {
    if (SaveResult slot = validate_slot(slot_id); !slot.ok) {
        return {.result = slot};
    }
    const std::filesystem::path path = slot_path(slot_id);
    if (!std::filesystem::exists(path)) {
        return load_error(SaveErrorCode::NotFound, "save slot not found", path);
    }
    bool read_ok = false;
    const std::string text = read_text_file(path, read_ok);
    if (!read_ok) {
        return load_error(SaveErrorCode::IoError, "failed to read save slot", path);
    }
    JsonParseResult parsed = with_payload ? parse_json(text) : parse_json_skipping(text, "payload");
    if (!parsed.ok()) {
        return load_error(SaveErrorCode::ParseError, parsed.error, path);
    }
    JsonValue& root = *parsed.value;
    if (SaveResult envelope = validate_envelope(root, save_schema, _game_id, path); !envelope.ok) {
        return {.result = envelope};
    }
    if (root.string_at("slot_id") != slot_id) {
        return load_error(SaveErrorCode::SchemaMismatch, "save slot id does not match path", path);
    }

    SaveLoadResult result;
    result.result = ok_result(path);
    result.info = parse_slot_info(root);
    if (std::optional<JsonValue> payload = root.take_member("payload")) {
        result.payload = std::move(*payload);
    }
    return result;
}

std::vector<SaveSlotInfo> SaveStore::list_slots() const {
    std::vector<SaveSlotInfo> slots;
    if (!validate_store().ok) {
        return slots;
    }
    const std::filesystem::path saves_dir = _root / "saves";
    if (!std::filesystem::exists(saves_dir)) {
        return slots;
    }
    std::error_code ec;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator{saves_dir, ec}) {
        if (ec || !entry.is_regular_file() || !has_json_extension(entry.path()) || is_temp_path(entry.path())) {
            continue;
        }
        const std::string slot_id = entry.path().stem().string();
        if (!valid_save_id(slot_id)) {
            continue;
        }
        SaveLoadResult loaded = read_slot_file(slot_id, false);  // the info alone
        if (loaded.result.ok) {
            slots.push_back(std::move(loaded.info));
        }
    }
    std::ranges::sort(slots, {}, &SaveSlotInfo::slot_id);
    return slots;
}

SaveResult SaveStore::delete_slot(std::string_view slot_id) {
    if (SaveResult slot = validate_slot(slot_id); !slot.ok) {
        return slot;
    }
    const std::filesystem::path path = slot_path(slot_id);
    if (!std::filesystem::exists(path)) {
        return error_result(SaveErrorCode::NotFound, "save slot not found", path);
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
    if (ec) {
        return error_result(SaveErrorCode::IoError, "failed to delete save slot: " + ec.message(), path);
    }
    return ok_result(path);
}

bool SaveStore::slot_exists(std::string_view slot_id) const {
    return validate_slot(slot_id).ok && std::filesystem::exists(slot_path(slot_id));
}

} // namespace kin
