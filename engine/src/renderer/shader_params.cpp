#include <kin/renderer/shader_reflect.hpp>
#include <algorithm>

namespace kin {

bool ShaderParams::set(std::string_view name, std::span<const f32> values) {
    const ShaderParamInfo* param = layout ? layout->find(name) : nullptr;
    if (!param || values.size_bytes() > param->size || param->offset % 4 != 0) {
        return false;
    }
    const std::size_t first = param->offset / 4;
    if (uniforms.size() < first + values.size()) {
        uniforms.resize(first + values.size(), 0.0f);
    }
    std::copy(values.begin(), values.end(), uniforms.begin() + static_cast<std::ptrdiff_t>(first));
    return true;
}

const ShaderParamInfo* ShaderLayout::find(std::string_view name) const {
    for (const ShaderParamInfo& p : params) {
        if (p.name == name) {
            return &p;
        }
    }
    return nullptr;
}

} // namespace kin
