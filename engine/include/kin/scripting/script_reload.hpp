#pragma once

// Live reload of a scene script (ScriptEngine) as one part of a development
// reload (kin/runtime/development.hpp), and the swap it is made of.

#include <kin/assets/development_revision.hpp>
#include <kin/scripting/script_engine.hpp>

#include <filesystem>
#include <memory>
#include <string>

namespace kin {

// Switches the running script to a prepared one (ScriptEngine::prepare_reload):
// `active`'s systems are suspended, `next`'s registered, and the two swapped
// (`next` then holds the old script). False, with `error`, when `next`'s
// systems do not register; rollback_script undoes it either way.
bool activate_script(std::unique_ptr<ScriptEngine>& active, std::unique_ptr<ScriptEngine>& next, std::string& error);

// Makes the old script active again after activate_script (`swapped`: it
// succeeded). Should the old script's systems not register again, it stays
// with them removed and halted (ScriptEngine::halt): its calls fail with the
// reason, so the host reports it.
void rollback_script(std::unique_ptr<ScriptEngine>& active, std::unique_ptr<ScriptEngine>& next, bool swapped);

// The scene script `script` of `revision` prepared to replace `active`, which
// must outlive the change.
std::unique_ptr<DevelopmentChange> prepare_script_reload(std::unique_ptr<ScriptEngine>& active,
                                                         const DevelopmentRevision& revision,
                                                         const std::filesystem::path& script, std::string& error);

} // namespace kin
