#pragma once
#include <fstream>
#include <kin/runtime/scene_server.hpp>
#include <kin/ui2/state.hpp>

namespace kin {
// The endpoint descriptions are shared by discovery and the typed runner.
const JsonValue& automation_protocol();
std::optional<ServerResponse> dispatch_automation_command(ServerContext&, const GameInfo*, std::string_view,
                                                          const JsonValue&, bool initialize_ui = true);
std::optional<ui2::Id> automation_ui_selector(ServerContext&, const JsonValue&, std::string& error);
struct AutomationRecorder {
    AutomationRecorder(const std::filesystem::path&, ServerContext&, const GameInfo*, std::optional<u64>);
    void command(ServerContext&, std::string_view, const JsonValue&, const ServerResponse&);
    void begin_command(ServerContext&, std::string_view, const JsonValue&);
    void frame(ServerContext&);
    bool good() const;
    std::string error;

private:
    std::ofstream stream;
    u64 sequence = 0, bytes = 0;
    std::vector<u64> pending;
    void append(const JsonValue&);
    void append_text(const std::string&);
};
} // namespace kin
