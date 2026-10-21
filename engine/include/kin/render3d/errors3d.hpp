#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kin {

using Diagnostics3D = std::vector<std::string>;

// Fallible 3D loading, rendering and authoring operations share these results. Successful
// results have no diagnostics. A valid empty query/value is still success.
// Inspect before dereferencing; values/diagnostics survive later owner calls.
template <class T> struct [[nodiscard]] Result3D {
    std::optional<T> value;
    Diagnostics3D diagnostics;
    bool ok() const { return value.has_value() && diagnostics.empty(); }
    explicit operator bool() const { return ok(); }
    T& operator*() { return value.value(); }
    const T& operator*() const { return value.value(); }
    T* operator->() { return &value.value(); }
    const T* operator->() const { return &value.value(); }
    T value_or(T fallback) const& { return ok() ? *value : std::move(fallback); }
    T value_or(T fallback) && { return ok() ? std::move(*value) : std::move(fallback); }
};
template <> struct Result3D<void> {
    bool succeeded = true;
    Diagnostics3D diagnostics;
    bool ok() const { return succeeded && diagnostics.empty(); }
    explicit operator bool() const { return ok(); }
};
using Status3D = Result3D<void>;
inline Status3D status3d(bool succeeded, Diagnostics3D diagnostics = {}) {
    if (!succeeded && diagnostics.empty())
        diagnostics.emplace_back("3D operation refused");
    return {succeeded, std::move(diagnostics)};
}
template <class T> Result3D<T> result3d(T value, Diagnostics3D diagnostics = {}) {
    if (!diagnostics.empty())
        return {std::nullopt, std::move(diagnostics)};
    return {std::move(value), {}};
}

// One contextual runtime failure, allocated only on failure. Scene submission
// can latch it until clear; loading/renderer operations replace it each call.
inline bool fail3d(Diagnostics3D& errors, std::string_view operation, std::string_view reason) {
    errors.clear();
    errors.emplace_back(std::string(operation) + ": " + std::string(reason));
    return false;
}

inline std::string diagnostics3d_text(const Diagnostics3D& errors) {
    std::string text;
    for (const auto& error : errors) {
        if (!text.empty())
            text += '\n';
        text += error;
    }
    return text;
}

} // namespace kin
