#pragma once

#include <string>
#include <utility>

namespace kin {

// Shared sink a headless run uses to signal pass/fail. Scenes reach it through
// SceneContext::report and call fail() to abort the run with a non-zero process
// exit code and a reason recorded in the JSON run report. The first failure
// wins so the original cause is preserved.
struct RunReport {
    bool failed = false;
    std::string failure_reason;

    void fail(std::string reason) {
        if (!failed) {
            failed = true;
            failure_reason = std::move(reason);
        }
    }
};

} // namespace kin
