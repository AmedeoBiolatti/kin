#pragma once
#include <kin/runtime/development.hpp>
#include <kin/scripting/script_engine.hpp>
namespace kin {
std::unique_ptr<DevelopmentChange> prepare_script_reload(std::unique_ptr<ScriptEngine>& active,
                                                         const DevelopmentRevision& revision,
                                                         const std::filesystem::path& script, std::string& error);
}
