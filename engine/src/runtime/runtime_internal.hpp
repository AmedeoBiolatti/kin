#pragma once

#include <kin/assets/asset_server.hpp>
#include <kin/core/json_value.hpp>
#include <kin/runtime/scene_server.hpp>
#include <kin/scene/scene_manager.hpp>
#if KIN_ENABLE_DEVELOPMENT
#include <kin/runtime/development.hpp>
#endif

#include <charconv>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>

namespace kin {
struct SceneAppConfig;
class SceneManager;
struct StateCoverage;
} // namespace kin

namespace kin::runtime_detail {

#ifdef KIN_ENABLE_DETERMINISM_CHECK
// --check-determinism: runs the game several times as child processes in
// lockstep and reports where their states part. Returns the exit code.
int run_determinism_check(const SceneAppConfig& config);

// A --state-lockstep child's side, after each update: prints the state's hash
// and waits for the parent's command. False when the parent says to stop.
bool state_lockstep_step(SceneManager& scenes, i32 frame, StateCoverage& coverage);
// Prints what the run's states held that could not be compared.
void state_lockstep_finish(const StateCoverage& coverage);
#endif

// Both front-ends advance scenes through the same asset-pump policy: rendered
// loops stay non-blocking, while headless/server loops drain so simulation
// steps see a complete asset graph.
inline void pump_scene_assets(AssetServer* asset_server, bool render) {
    if (asset_server == nullptr) {
        return;
    }
    asset_server->pump(render ? AssetServer::PumpMode::Budgeted
                              : AssetServer::PumpMode::DrainToQuiescent);
}

// Both front-ends advance development sessions the same way, before a step:
// queued reloads apply at `tick`; windowed runs also watch the content folder.
inline void advance_development(SceneManager& scenes, u64 tick, bool watch_disk) {
#if KIN_ENABLE_DEVELOPMENT
    for (i32 i = 0; i < scenes.depth(); ++i) {
        if (DevelopmentSession* session = scenes.at_mut(i)->development_session()) {
            session->advance(tick, watch_disk);
        }
    }
#else
    (void)scenes;
    (void)tick;
    (void)watch_disk;
#endif
}

// An unsigned decimal integer, the whole of `text`.
inline bool parse_u64(std::string_view text, u64& value) {
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto parsed = std::from_chars(begin, end, value);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

// A JSON number that is an exact integer in [lo, hi]; nullopt otherwise.
inline std::optional<i64> exact_integer(const JsonValue& value, i64 lo, i64 hi) {
    if (!value.is_number()) {
        return std::nullopt;
    }
    const f64 number = value.as_number();
    if (!std::isfinite(number) || std::floor(number) != number || number < static_cast<f64>(lo) ||
        number > static_cast<f64>(hi)) {
        return std::nullopt;
    }
    return static_cast<i64>(number);
}

// Lower-case hex of the bytes.
inline std::string hex_encode(std::string_view bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const char c : bytes) {
        const auto byte = static_cast<unsigned char>(c);
        out.push_back(digits[byte >> 4]);
        out.push_back(digits[byte & 15]);
    }
    return out;
}

// The bytes `text` spells in hex digits (either case); nullopt if it is not an
// even count of them.
inline std::optional<std::string> hex_decode(std::string_view text) {
    if (text.size() % 2 != 0) {
        return std::nullopt;
    }
    const auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        return -1;
    };
    std::string bytes;
    bytes.reserve(text.size() / 2);
    for (std::size_t i = 0; i < text.size(); i += 2) {
        const int high = digit(text[i]);
        const int low = digit(text[i + 1]);
        if (high < 0 || low < 0) {
            return std::nullopt;
        }
        bytes.push_back(static_cast<char>(high * 16 + low));
    }
    return bytes;
}

// The scene server's command results.
inline ServerResponse ok_result(std::string json) {
    return {.ok = true, .error = {}, .result_json = std::move(json)};
}

inline ServerResponse error_result(std::string message) {
    return {.ok = false, .error = std::move(message), .result_json = {}};
}

} // namespace kin::runtime_detail
