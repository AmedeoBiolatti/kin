#pragma once

#include <kin/runtime/scene_server.hpp>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

namespace kin {

// The kin.automation/1 method descriptions (server.schema): what discovery
// reports and the scenario runner validates requests and results against.
const JsonValue& automation_protocol();

// The automation methods (server.*, automation.snapshot, world.query, ui.*,
// dev.*); nullopt for the others. With `initialize_ui` false, UI reads use the
// last layout instead of laying out a scene that has none yet (the inspector).
std::optional<ServerResponse> dispatch_automation_command(ServerContext& ctx, std::string_view method,
                                                          const JsonValue& params, bool initialize_ui = true);

// Writes a kin.automation-recording/1 JSONL file: a header, then each command
// (a request line before it runs, a command line with its result after) and
// each step's checkpoint.
class AutomationRecorder {
public:
    AutomationRecorder(const std::filesystem::path& path, ServerContext& ctx, const GameInfo* game,
                       std::optional<u64> seed);
    void begin_command(ServerContext& ctx, std::string_view method, const JsonValue& params);
    void command(ServerContext& ctx, std::string_view method, const JsonValue& params, const ServerResponse& response);
    void frame(ServerContext& ctx);
    bool good() const { return _error.empty() && _stream.good(); }
    const std::string& error() const { return _error; }

private:
    void append(const JsonValue& value);
    void append_text(const std::string& line);

    std::ofstream _stream;
    std::string _error;
    u64 _sequence = 0;
    u64 _bytes = 0;
    std::optional<u64> _running; // the command between begin_command and command
};

} // namespace kin
