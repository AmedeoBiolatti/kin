#pragma once
#include <kin/render3d/errors3d.hpp>
namespace kin::test {
template <class T> bool read_value3d(Result3D<T> result, T& output, std::string& why) {
    why = diagnostics3d_text(result.diagnostics);
    if (result.ok())
        output = std::move(*result);
    return result.ok();
}
inline bool valid3d(Status3D result, std::string& why) {
    why = diagnostics3d_text(result.diagnostics);
    return result.ok();
}
template <class T> T value3d(Result3D<T> result, std::string& why) {
    why = diagnostics3d_text(result.diagnostics);
    return std::move(result).value_or({});
}
} // namespace kin::test
