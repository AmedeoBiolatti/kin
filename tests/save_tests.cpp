#include <kin/platform/user_data.hpp>
#include <kin/core/json_value.hpp>
#include <kin/save/save_store.hpp>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::filesystem::path unique_temp_root(std::string_view name) {
    const auto stamp = kin::current_save_time_unix();
    std::filesystem::path path = std::filesystem::temp_directory_path() /
                                 ("kin-save-tests-" + std::string{name} + "-" + std::to_string(stamp));
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
}

void write_text(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    out << text;
}

void test_user_data_override_and_roots() {
    const std::filesystem::path temp = unique_temp_root("roots");
    kin::set_user_data_dir_override(temp);
    assert(kin::user_data_dir("kin") == temp / "kin");

    kin::SaveStore default_store{{.game_id = "sample_game", .game_version = "0.1"}};
    assert(default_store.root() == temp / "kin" / "sample_game");

    kin::SaveStore override_store{{
        .game_id = "sample_game",
        .game_version = "0.1",
        .root_override = temp / "direct-root",
    }};
    assert(override_store.root() == temp / "direct-root");

    kin::clear_user_data_dir_override();
    std::filesystem::remove_all(temp);
}

void test_id_validation() {
    assert(kin::valid_save_id("slot_1-A"));
    assert(!kin::valid_save_id(""));
    assert(!kin::valid_save_id("."));
    assert(!kin::valid_save_id(".."));
    assert(!kin::valid_save_id("bad id"));
    assert(!kin::valid_save_id("../bad"));
    assert(!kin::valid_save_id("C:bad"));

    const std::filesystem::path temp = unique_temp_root("ids");
    kin::SaveStore bad_game{{
        .game_id = "bad game",
        .game_version = "0.1",
        .root_override = temp / "bad",
    }};
    assert(bad_game.write_settings(nullptr).error == kin::SaveErrorCode::InvalidId);

    kin::SaveStore store{{
        .game_id = "game",
        .game_version = "0.1",
        .root_override = temp / "game",
    }};
    kin::SaveSlotInfo info;
    assert(store.write_slot("bad id", info, nullptr).error == kin::SaveErrorCode::InvalidId);
    assert(store.read_slot("../bad").result.error == kin::SaveErrorCode::InvalidId);
    std::filesystem::remove_all(temp);
}

void test_settings() {
    const std::filesystem::path temp = unique_temp_root("settings");
    kin::set_save_time_override(1760000000);
    kin::SaveStore store{{
        .game_id = "settings_game",
        .game_version = "0.1",
        .root_override = temp / "settings_game",
    }};

    kin::SaveLoadResult missing = store.read_settings();
    assert(!missing.result.ok);
    assert(missing.result.error == kin::SaveErrorCode::NotFound);

    kin::SaveResult written = store.write_settings([](kin::JsonWriter& json) {
        json.begin_object();
        json.field("master_volume", 0.8);
        json.field("fullscreen", false);
        json.end_object();
    });
    assert(written.ok);
    assert(std::filesystem::exists(store.settings_path()));

    kin::SaveLoadResult loaded = store.read_settings();
    assert(loaded.result.ok);
    assert(loaded.info.game_version == "0.1");
    assert(loaded.info.updated_at_unix == 1760000000);
    assert(loaded.payload.number_at("master_volume") == 0.8);
    assert(!loaded.payload.bool_at("fullscreen", true));

    const std::string raw = [] (const std::filesystem::path& path) {
        std::ifstream in{path, std::ios::binary};
        return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    }(store.settings_path());
    assert(raw.find("\"schema\": \"kin.settings/1\"") != std::string::npos);
    assert(raw.find("\"game_id\": \"settings_game\"") != std::string::npos);

    write_text(store.settings_path(), "{bad");
    kin::SaveLoadResult corrupt = store.read_settings();
    assert(!corrupt.result.ok);
    assert(corrupt.result.error == kin::SaveErrorCode::ParseError);

    kin::clear_save_time_override();
    std::filesystem::remove_all(temp);
}

void test_slots() {
    const std::filesystem::path temp = unique_temp_root("slots");
    kin::set_save_time_override(1760000100);
    kin::SaveStore store{{
        .game_id = "slot_game",
        .game_version = "0.2",
        .root_override = temp / "slot_game",
    }};

    kin::SaveSlotInfo info;
    info.title = "Before the boss";
    info.created_at_unix = 1760000000;
    info.play_time_seconds = 1200;
    info.payload_version = 99;

    assert(store.write_slot("slot_b", info, [](kin::JsonWriter& json) {
        json.begin_object().field("score", 4200).field("level", "dungeon_03").end_object();
    }).ok);
    assert(store.write_slot("slot_a", info, [](kin::JsonWriter& json) {
        json.begin_object().field("score", 7).end_object();
    }).ok);
    write_text(store.root() / "saves" / "ignored.json.tmp", "{}");

    assert(store.slot_exists("slot_b"));
    kin::SaveLoadResult loaded = store.read_slot("slot_b");
    assert(loaded.result.ok);
    assert(loaded.info.slot_id == "slot_b");
    assert(loaded.info.title == "Before the boss");
    assert(loaded.info.game_version == "0.2");
    assert(loaded.info.created_at_unix == 1760000000);
    assert(loaded.info.updated_at_unix == 1760000100);
    assert(loaded.info.play_time_seconds == 1200);
    assert(loaded.info.payload_version == 99);
    assert(loaded.payload.int_at("score") == 4200);
    assert(loaded.payload.string_at("level") == "dungeon_03");

    const std::vector<kin::SaveSlotInfo> slots = store.list_slots();
    assert(slots.size() == 2);
    assert(slots[0].slot_id == "slot_a");
    assert(slots[1].slot_id == "slot_b");

    kin::SaveResult deleted = store.delete_slot("slot_a");
    assert(deleted.ok);
    assert(!store.slot_exists("slot_a"));
    assert(store.delete_slot("slot_a").error == kin::SaveErrorCode::NotFound);

    kin::clear_save_time_override();
    std::filesystem::remove_all(temp);
}

void test_load_errors() {
    const std::filesystem::path temp = unique_temp_root("errors");
    kin::SaveStore store{{
        .game_id = "real_game",
        .game_version = "0.1",
        .root_override = temp / "real_game",
    }};

    write_text(store.settings_path(), R"({
  "schema": "kin.settings/1",
  "game_id": "other_game",
  "game_version": "0.1",
  "updated_at": 1,
  "payload": {}
})");
    assert(store.read_settings().result.error == kin::SaveErrorCode::GameMismatch);

    write_text(store.settings_path(), R"({
  "schema": "kin.settings/2",
  "game_id": "real_game",
  "game_version": "0.1",
  "updated_at": 1,
  "payload": {}
})");
    assert(store.read_settings().result.error == kin::SaveErrorCode::UnsupportedVersion);

    std::filesystem::remove_all(temp);
}

void test_manual_game_state_roundtrip() {
    struct GameState {
        kin::i32 score = 0;
        std::string level;
        kin::f64 x = 0.0;
        kin::f64 y = 0.0;
    };

    const std::filesystem::path temp = unique_temp_root("roundtrip");
    kin::SaveStore store{{
        .game_id = "roundtrip_game",
        .game_version = "0.1",
        .root_override = temp / "roundtrip_game",
    }};

    const GameState saved{.score = 42, .level = "dungeon_03", .x = 12.5, .y = -4.0};
    kin::SaveSlotInfo info{.title = "Manual state"};
    assert(store.write_slot("slot1", info, [&](kin::JsonWriter& json) {
        json.begin_object();
        json.field("score", saved.score);
        json.field("level", std::string_view{saved.level});
        json.key("player").begin_object();
        json.field("x", saved.x);
        json.field("y", saved.y);
        json.end_object();
        json.end_object();
    }).ok);

    kin::SaveLoadResult loaded = store.read_slot("slot1");
    assert(loaded.result.ok);
    GameState restored;
    restored.score = static_cast<kin::i32>(loaded.payload.int_at("score"));
    restored.level = loaded.payload.string_at("level");
    const kin::JsonValue* player = loaded.payload.find("player");
    assert(player != nullptr);
    restored.x = player->number_at("x");
    restored.y = player->number_at("y");

    assert(restored.score == saved.score);
    assert(restored.level == saved.level);
    assert(restored.x == saved.x);
    assert(restored.y == saved.y);

    std::filesystem::remove_all(temp);
}

} // namespace

// Compact files: no indentation or line breaks, read back the same; the
// listing reads each slot's info and passes its payload over unread.
void test_compact_slots() {
    const std::filesystem::path temp = unique_temp_root("compact");
    kin::set_save_time_override(1760000200);
    kin::SaveStore store{{
        .game_id = "compact_game",
        .game_version = "1.0",
        .root_override = temp / "compact_game",
        .compact = true,
    }};
    kin::SaveSlotInfo info;
    info.title = "Compact";
    info.summary = "seed 54";
    assert(store.write_slot("small", info, [](kin::JsonWriter& json) {
        json.begin_object().field("note", "a [bracket] and a \"quote\" {brace}");
        json.key("cells").begin_array();
        for (int i = 0; i < 1000; ++i) {
            json.value(i);
        }
        json.end_array().end_object();
    }).ok);
    std::string text;
    {
        std::ifstream in{store.slot_path("small"), std::ios::binary};  // closed before the files go
        text.assign(std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{});
    }
    assert(text.find('\n') == text.size() - 1);  // one line, then its end
    assert(text.find("  ") == std::string::npos);
    const kin::SaveLoadResult loaded = store.read_slot("small");
    assert(loaded.result.ok);
    assert(loaded.payload.string_at("note") == "a [bracket] and a \"quote\" {brace}");
    assert(loaded.payload.find("cells")->items().size() == 1000);
    assert(loaded.payload.find("cells")->items()[999].as_int() == 999);

    // A payload that would not parse (but whose brackets close): listed by
    // its info, which is all the listing reads; refused when read whole.
    write_text(store.slot_path("broken"),
               R"({"schema":"kin.save/1","game_id":"compact_game","game_version":"1.0","slot_id":"broken",)"
               R"("title":"Listed","payload":{"cells":[1,2x,3],"s":"]}"},"updated_at":5})");
    const std::vector<kin::SaveSlotInfo> slots = store.list_slots();
    assert(slots.size() == 2);
    assert(slots[0].slot_id == "broken" && slots[0].title == "Listed" && slots[0].updated_at_unix == 5);
    assert(slots[0].summary.empty() && slots[1].summary == "seed 54");  // the listing's own line, unread payloads
    assert(!store.read_slot("broken").result.ok);

    // Pretty files (an older store's) read the same.
    kin::SaveStore pretty{{.game_id = "compact_game", .game_version = "1.0", .root_override = temp / "compact_game"}};
    assert(pretty.write_slot("pretty", info, [](kin::JsonWriter& json) { json.begin_object().field("n", 3).end_object(); }).ok);
    assert(store.read_slot("pretty").payload.int_at("n") == 3);

    kin::clear_save_time_override();
    std::filesystem::remove_all(temp);
}

// The value model: small values, deep copies, members moved out, and the
// parser passing over a root member (only the root's) without building it.
void test_json_values() {
    static_assert(sizeof(kin::JsonValue) <= 32);
    kin::JsonParseResult parsed = kin::parse_json(R"({"a":[1,{"b":"x"}],"keep":{"payload":7},"payload":{"big":[1,2,3]}})");
    assert(parsed.ok());
    kin::JsonValue copy = *parsed.value;
    std::optional<kin::JsonValue> taken = parsed.value->take_member("payload");
    assert(taken && taken->find("big")->items().size() == 3);
    assert(parsed.value->find("payload")->is_null());   // moved out
    assert(copy.find("payload")->find("big")->items()[2].as_int() == 3);  // the copy kept its own
    assert(!parsed.value->take_member("missing"));
    assert(copy.find("a")->items()[1].string_at("b") == "x");

    const kin::JsonParseResult skipped =
        kin::parse_json_skipping(R"({"payload":{"s":"\"}]","n":[[1],[2]]},"keep":{"payload":7},"after":true})", "payload");
    assert(skipped.ok());
    assert(!skipped.value->find("payload"));
    assert(skipped.value->find("keep")->int_at("payload") == 7);  // not the root's: kept
    assert(skipped.value->bool_at("after"));
    assert(kin::parse_json_skipping(R"({"payload":3,"x":1})", "payload").value->int_at("x") == 1);
    assert(!kin::parse_json_skipping(R"({"payload":{"unclosed":[1,2})", "payload").ok());

    kin::JsonValue from{std::string{"moved"}};
    const kin::JsonValue to = std::move(from);
    assert(to.as_string() == "moved");
    assert(from.is_null() && from.as_string().empty());  // NOLINT(bugprone-use-after-move): left null

    const kin::JsonValue number{3.5};
    assert(number.items().empty() && number.members().empty() && number.as_string().empty());
}

int main() {
    test_user_data_override_and_roots();
    test_id_validation();
    test_settings();
    test_slots();
    test_load_errors();
    test_manual_game_state_roundtrip();
    test_compact_slots();
    test_json_values();
    return 0;
}
