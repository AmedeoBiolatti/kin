#pragma once

#include <kin/core/json.hpp>
#include <kin/core/json_value.hpp>
#include <kin/core/types.hpp>

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace kin {

class SaveStore;

struct SaveStoreConfig {
    std::string game_id;
    std::string game_version;
    std::filesystem::path root_override;
};

enum class SaveErrorCode {
    None,
    NotFound,
    InvalidId,
    IoError,
    ParseError,
    SchemaMismatch,
    GameMismatch,
    UnsupportedVersion,
};

struct SaveResult {
    bool ok = false;
    SaveErrorCode error = SaveErrorCode::None;
    std::string message;
    std::filesystem::path path;
};

struct SaveSlotInfo {
    std::string slot_id;
    std::string title;
    std::string game_version;
    i64 created_at_unix = 0;
    i64 updated_at_unix = 0;
    i64 play_time_seconds = 0;
    i32 payload_version = 1;
};

struct SaveLoadResult {
    SaveResult result;
    SaveSlotInfo info;
    JsonValue payload;
};

void set_save_time_override(i64 unix_seconds);
void clear_save_time_override();
i64 current_save_time_unix();
bool valid_save_id(std::string_view id);

class SaveStore {
public:
    explicit SaveStore(SaveStoreConfig config);

    const std::filesystem::path& root() const { return _root; }
    std::filesystem::path settings_path() const;
    std::filesystem::path slot_path(std::string_view slot_id) const;

    SaveResult write_settings(std::function<void(JsonWriter&)> write_payload);
    SaveLoadResult read_settings() const;

    SaveResult write_slot(std::string_view slot_id,
                          const SaveSlotInfo& info,
                          std::function<void(JsonWriter&)> write_payload);

    SaveLoadResult read_slot(std::string_view slot_id) const;
    std::vector<SaveSlotInfo> list_slots() const;
    SaveResult delete_slot(std::string_view slot_id);
    bool slot_exists(std::string_view slot_id) const;

private:
    SaveResult validate_store() const;
    SaveResult validate_slot(std::string_view slot_id) const;

    std::string _game_id;
    std::string _game_version;
    std::filesystem::path _root;
    bool _valid_game_id = false;
};

} // namespace kin
