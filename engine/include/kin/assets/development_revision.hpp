#pragma once

#include <kin/assets/content.hpp>

#include <functional>
#include <memory>
#include <string>

namespace kin {

// One immutable content snapshot a development session (kin/runtime/development.hpp)
// reloads from, and its identity: a hash of its paths, sizes and file hashes.
struct DevelopmentRevision {
    std::shared_ptr<const MemoryContent> content;
    std::string id;
};

// One part of a development reload, prepared from a revision without changing
// live state. Activation and rollback run on the owning thread, between
// simulation steps: when any part fails to activate, the parts already active
// roll back in reverse order. finalize is no-fail ownership cleanup, run after
// every part activated.
struct DevelopmentChange {
    std::function<bool(std::string&)> activate;
    std::function<void()> rollback;
    std::function<void()> finalize;
};

} // namespace kin
