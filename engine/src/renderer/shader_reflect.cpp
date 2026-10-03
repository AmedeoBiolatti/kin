#include <kin/renderer/shader_reflect.hpp>

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace kin {

namespace {

// The few SPIR-V opcodes, decorations and storage classes this reads.
enum Op : u32 {
    OpName = 5,
    OpMemberName = 6,
    OpTypeInt = 21,
    OpTypeFloat = 22,
    OpTypeVector = 23,
    OpTypeMatrix = 24,
    OpTypeImage = 25,
    OpTypeSampledImage = 27,
    OpTypeArray = 28,
    OpTypeRuntimeArray = 29,
    OpTypeStruct = 30,
    OpTypePointer = 32,
    OpConstant = 43,
    OpVariable = 59,
    OpDecorate = 71,
    OpMemberDecorate = 72,
};
enum Decoration : u32 {
    Block = 2,
    BufferBlock = 3,
    ArrayStride = 6,
    MatrixStride = 7,
    Binding = 33,
    DescriptorSet = 34,
    Offset = 35,
};
enum StorageClass : u32 {
    UniformConstant = 0,
    Uniform = 2,
    StorageBuffer = 12,
};

struct Type {
    u32 op = 0;
    std::vector<u32> operands; // after the result id
};

std::string read_string(const u32* words, std::size_t count) {
    std::string out;
    for (std::size_t i = 0; i < count; ++i) {
        for (int b = 0; b < 4; ++b) {
            const char c = static_cast<char>((words[i] >> (8 * b)) & 0xFF);
            if (c == '\0') {
                return out;
            }
            out.push_back(c);
        }
    }
    return out;
}

} // namespace

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

std::optional<ShaderLayout> reflect_spirv(ShaderBlob spirv, std::string* error) {
    const auto fail = [&](const char* why) -> std::optional<ShaderLayout> {
        if (error) {
            *error = why;
        }
        return std::nullopt;
    };
    if (!spirv.valid() || spirv.size % 4 != 0 || spirv.size < 20) {
        return fail("not a SPIR-V module (size)");
    }
    std::vector<u32> words(spirv.size / 4);
    std::memcpy(words.data(), spirv.code, spirv.size);
    if (words[0] != 0x07230203u) {
        return fail("not a SPIR-V module (magic)");
    }

    std::unordered_map<u32, Type> types;
    std::unordered_map<u32, u32> constants;                     // id -> value (32-bit ints)
    std::unordered_map<u32, std::unordered_map<u32, u32>> decorations; // id -> decoration -> value
    std::unordered_map<u32, std::unordered_map<u32, u32>> member_offsets; // struct -> member -> offset
    std::unordered_map<u32, std::unordered_map<u32, std::string>> member_names;
    struct Variable {
        u32 type = 0; // a pointer type
        u32 storage = 0;
    };
    std::unordered_map<u32, Variable> variables;

    for (std::size_t at = 5; at < words.size();) {
        const u32 count = words[at] >> 16;
        const u32 op = words[at] & 0xFFFF;
        if (count == 0 || at + count > words.size()) {
            return fail("truncated SPIR-V instruction");
        }
        const u32* w = &words[at];
        switch (op) {
        case OpMemberName:
            member_names[w[1]][w[2]] = read_string(w + 3, count - 3);
            break;
        case OpTypeInt: case OpTypeFloat: case OpTypeVector: case OpTypeMatrix: case OpTypeImage:
        case OpTypeSampledImage: case OpTypeArray: case OpTypeRuntimeArray: case OpTypeStruct:
        case OpTypePointer:
            types[w[1]] = Type{op, std::vector<u32>(w + 2, w + count)};
            break;
        case OpConstant:
            if (count >= 4) {
                constants[w[2]] = w[3];
            }
            break;
        case OpVariable:
            variables[w[2]] = Variable{w[1], w[3]};
            break;
        case OpDecorate:
            decorations[w[1]][w[2]] = count > 3 ? w[3] : 1;
            break;
        case OpMemberDecorate:
            if (w[3] == Offset && count > 4) {
                member_offsets[w[1]][w[2]] = w[4];
            }
            break;
        default:
            break;
        }
        at += count;
    }

    const auto type_of = [&](u32 id) -> const Type* {
        const auto it = types.find(id);
        return it == types.end() ? nullptr : &it->second;
    };
    const auto decoration = [&](u32 id, u32 which) -> std::optional<u32> {
        const auto it = decorations.find(id);
        if (it == decorations.end()) {
            return std::nullopt;
        }
        const auto d = it->second.find(which);
        return d == it->second.end() ? std::nullopt : std::optional<u32>{d->second};
    };
    // A type's size in a std140/std430 block, when it can be known.
    const auto size_of = [&](const auto& self, u32 id) -> u32 {
        const Type* t = type_of(id);
        if (!t) {
            return 0;
        }
        switch (t->op) {
        case OpTypeInt: case OpTypeFloat:
            return t->operands.empty() ? 4 : t->operands[0] / 8;
        case OpTypeVector:
            return self(self, t->operands[0]) * t->operands[1];
        case OpTypeMatrix: {
            const u32 stride = decoration(id, MatrixStride).value_or(16);
            return stride * t->operands[1];
        }
        case OpTypeArray: {
            const u32 length = constants.contains(t->operands[1]) ? constants[t->operands[1]] : 0;
            const u32 stride = decoration(id, ArrayStride).value_or(self(self, t->operands[0]));
            return stride * length;
        }
        case OpTypeStruct: {
            u32 end = 0;
            for (u32 m = 0; m < t->operands.size(); ++m) {
                const u32 offset = member_offsets[id].contains(m) ? member_offsets[id][m] : end;
                end = std::max(end, offset + self(self, t->operands[m]));
            }
            return end;
        }
        default:
            return 0;
        }
    };

    ShaderLayout layout;
    std::unordered_set<u32> sampler_bindings, storage_texture_bindings, storage_buffer_bindings, uniform_bindings;
    for (const auto& [id, variable] : variables) {
        const Type* pointer = type_of(variable.type);
        if (!pointer || pointer->op != OpTypePointer || pointer->operands.size() < 2) {
            continue;
        }
        u32 pointee = pointer->operands[1];
        const Type* t = type_of(pointee);
        // Arrays of resources count as one slot per element.
        u32 elements = 1;
        if (t && t->op == OpTypeArray) {
            elements = constants.contains(t->operands[1]) ? constants[t->operands[1]] : 1;
            pointee = t->operands[0];
            t = type_of(pointee);
        }
        if (!t) {
            continue;
        }
        const u32 binding = decoration(id, Binding).value_or(0);
        if (variable.storage == UniformConstant) {
            if (t->op == OpTypeSampledImage) {
                for (u32 e = 0; e < elements; ++e) sampler_bindings.insert(binding + e);
            } else if (t->op == OpTypeImage && t->operands.size() > 5 && t->operands[5] == 2) {
                for (u32 e = 0; e < elements; ++e) storage_texture_bindings.insert(binding + e);
            }
        } else if (variable.storage == StorageBuffer ||
                   (variable.storage == Uniform && decoration(pointee, BufferBlock))) {
            storage_buffer_bindings.insert(binding);
        } else if (variable.storage == Uniform && t->op == OpTypeStruct) {
            uniform_bindings.insert(binding);
            if (binding == 0) {
                layout.uniform_bytes = size_of(size_of, pointee);
                for (u32 m = 0; m < t->operands.size(); ++m) {
                    const auto name = member_names[pointee].find(m);
                    layout.params.push_back(ShaderParamInfo{
                        .name = name == member_names[pointee].end() ? std::string{} : name->second,
                        .offset = member_offsets[pointee].contains(m) ? member_offsets[pointee][m] : 0,
                        .size = size_of(size_of, t->operands[m])});
                }
            }
        }
    }
    layout.samplers = static_cast<u32>(sampler_bindings.size());
    layout.storage_textures = static_cast<u32>(storage_texture_bindings.size());
    layout.storage_buffers = static_cast<u32>(storage_buffer_bindings.size());
    layout.uniform_buffers = static_cast<u32>(uniform_bindings.size());
    return layout;
}

} // namespace kin
