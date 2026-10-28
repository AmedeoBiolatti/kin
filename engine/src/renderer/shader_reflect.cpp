#include <algorithm>
#include <cstring>
#include <kin/renderer/shader_reflect.hpp>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace kin {

namespace {

// The few SPIR-V opcodes, decorations and storage classes this reads.
enum Op : u32 {
    OpName = 5,
    OpEntryPoint = 15,
    OpExecutionMode = 16,
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
    NonWritable = 24,
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
    std::unordered_map<u32, std::unordered_set<u32>> non_writable_members;
    u32 execution_model = ~u32{0};
    std::unordered_map<u32, std::unordered_map<u32, u32>> matrix_strides;
    std::unordered_map<u32, std::unordered_set<u32>> row_major;
    struct Variable {
        u32 type = 0; // a pointer type
        u32 storage = 0;
    };
    std::unordered_map<u32, Variable> variables;
    u32 layout_local_size[3]{1, 1, 1};

    for (std::size_t at = 5; at < words.size();) {
        const u32 count = words[at] >> 16;
        const u32 op = words[at] & 0xFFFF;
        if (count == 0 || at + count > words.size()) {
            return fail("truncated SPIR-V instruction");
        }
        const u32* w = &words[at];
        switch (op) {
        case OpEntryPoint:
            if (count < 4)
                return fail("invalid SPIR-V entry point");
            if (read_string(w + 3, count - 3) == "main")
                execution_model = w[1];
            break;
        case OpExecutionMode:
            if (count >= 6 && w[2] == 17) { // LocalSize x y z
                layout_local_size[0] = w[3];
                layout_local_size[1] = w[4];
                layout_local_size[2] = w[5];
            }
            break;
        case OpMemberName:
            if (count < 4)
                return fail("invalid SPIR-V member name");
            member_names[w[1]][w[2]] = read_string(w + 3, count - 3);
            break;
        case OpTypeInt: case OpTypeFloat: case OpTypeVector: case OpTypeMatrix: case OpTypeImage:
        case OpTypeSampledImage: case OpTypeArray: case OpTypeRuntimeArray: case OpTypeStruct:
        case OpTypePointer:
            if (count < 3)
                return fail("invalid SPIR-V type");
            types[w[1]] = Type{op, std::vector<u32>(w + 2, w + count)};
            break;
        case OpConstant:
            if (count >= 4) {
                constants[w[2]] = w[3];
            }
            break;
        case OpVariable:
            if (count < 4)
                return fail("invalid SPIR-V variable");
            variables[w[2]] = Variable{w[1], w[3]};
            break;
        case OpDecorate:
            if (count < 3)
                return fail("invalid SPIR-V decoration");
            decorations[w[1]][w[2]] = count > 3 ? w[3] : 1;
            break;
        case OpMemberDecorate:
            if (count < 4)
                return fail("invalid SPIR-V member decoration");
            if (w[3] == NonWritable)
                non_writable_members[w[1]].insert(w[2]);
            if (w[3] == MatrixStride && count > 4)
                matrix_strides[w[1]][w[2]] = w[4];
            if (w[3] == 4)
                row_major[w[1]].insert(w[2]);
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
    const auto saturated = [](u64 value) {
        return static_cast<u32>(std::min<u64>(value, std::numeric_limits<u32>::max()));
    };
    // A type's size in a std140/std430 block, when it can be known.
    const auto size_of = [&](const auto& self, u32 id, u32 depth = 0) -> u32 {
        if (depth > 64)
            return 0;
        const Type* t = type_of(id);
        if (!t) {
            return 0;
        }
        switch (t->op) {
        case OpTypeInt:
        case OpTypeFloat:
            return t->operands.empty() ? 4 : t->operands[0] / 8;
        case OpTypeVector:
            return t->operands.size() < 2 ? 0 : saturated(u64(self(self, t->operands[0], depth + 1)) * t->operands[1]);
        case OpTypeMatrix: {
            if (t->operands.size() < 2)
                return 0;
            const u32 stride = decoration(id, MatrixStride).value_or(16);
            return saturated(u64(stride) * t->operands[1]);
        }
        case OpTypeArray: {
            if (t->operands.size() < 2)
                return 0;
            const u32 length = constants.contains(t->operands[1]) ? constants[t->operands[1]] : 0;
            const u32 stride = decoration(id, ArrayStride).value_or(self(self, t->operands[0], depth + 1));
            return saturated(u64(stride) * length);
        }
        case OpTypeRuntimeArray:
            return t->operands.empty() ? 0
                                       : decoration(id, ArrayStride).value_or(self(self, t->operands[0], depth + 1));
        case OpTypeStruct: {
            u32 end = 0;
            for (u32 m = 0; m < t->operands.size(); ++m) {
                const u32 offset = member_offsets[id].contains(m) ? member_offsets[id][m] : end;
                u32 bytes = self(self, t->operands[m], depth + 1);
                const Type* member = type_of(t->operands[m]);
                if (member && member->op == OpTypeMatrix && member->operands.size() >= 2 &&
                    matrix_strides[id].contains(m)) {
                    u32 vectors = member->operands[1];
                    if (row_major[id].contains(m)) {
                        const Type* column = type_of(member->operands[0]);
                        if (column && column->operands.size() >= 2)
                            vectors = column->operands[1];
                    }
                    bytes = saturated(u64(matrix_strides[id][m]) * vectors);
                }
                end = std::max(end, saturated(u64(offset) + bytes));
            }
            return end;
        }
        default:
            return 0;
        }
    };

    ShaderLayout layout;
    layout.execution_model = execution_model;
    std::unordered_set<u32> sampler_bindings, storage_texture_bindings, storage_buffer_bindings, uniform_bindings;
    std::unordered_set<u32> rw_texture_bindings, rw_buffer_bindings;
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
            if (t->operands.size() < 2)
                return fail("invalid SPIR-V descriptor array");
            elements = constants.contains(t->operands[1]) ? constants[t->operands[1]] : 1;
            pointee = t->operands[0];
            t = type_of(pointee);
        }
        if (!t) {
            continue;
        }
        if (decoration(id, DescriptorSet) && (!elements || elements > MaxShaderSamplers))
            return fail("resource descriptor array exceeds supported limits");
        const u32 binding = decoration(id, Binding).value_or(0);
        if (decoration(id, DescriptorSet)) {
            ShaderResourceInfo resource;
            resource.set = *decoration(id, DescriptorSet);
            resource.binding = binding;
            resource.elements = elements;
            if (variable.storage == UniformConstant && t->op == OpTypeSampledImage && !t->operands.empty()) {
                resource.kind = ShaderResourceKind::SampledTexture;
                const Type* image = type_of(t->operands[0]);
                if (image && image->op == OpTypeImage && image->operands.size() >= 6) {
                    const auto& image_type = image->operands;
                    resource.dimensions = image_type[1] <= 2 ? image_type[1] + 1 : 0;
                    resource.comparison = image_type[2] == 1;
                    resource.arrayed = image_type[3] != 0;
                    resource.multisampled = image_type[4] != 0;
                    const Type* scalar = type_of(image_type[0]);
                    if (scalar && !scalar->operands.empty() && scalar->operands[0] == 32) {
                        if (scalar->op == OpTypeFloat)
                            resource.sample_type = ShaderSampleType::Float;
                        else if (scalar->op == OpTypeInt && scalar->operands.size() >= 2)
                            resource.sample_type =
                                scalar->operands[1] ? ShaderSampleType::Sint : ShaderSampleType::Uint;
                    }
                }
            } else if (variable.storage == StorageBuffer ||
                       (variable.storage == Uniform && decoration(pointee, BufferBlock))) {
                resource.kind = ShaderResourceKind::StorageBuffer;
                resource.min_bytes = size_of(size_of, pointee);
                resource.read_only = bool(decoration(id, NonWritable)) || bool(decoration(pointee, NonWritable)) ||
                                     (t->op == OpTypeStruct && !t->operands.empty() &&
                                      non_writable_members[pointee].size() == t->operands.size());
            } else if (variable.storage == Uniform && t->op == OpTypeStruct) {
                resource.kind = ShaderResourceKind::UniformBuffer;
                resource.min_bytes = size_of(size_of, pointee);
            } else if (variable.storage == UniformConstant && t->op == OpTypeImage)
                resource.kind = ShaderResourceKind::StorageTexture;
            layout.resources.push_back(resource);
        }

        // Set 1 holds what a compute shader writes (SDL_GPU's layout).
        const bool writes = decoration(id, DescriptorSet).value_or(0) == 1;
        if (variable.storage == UniformConstant) {
            if (t->op == OpTypeSampledImage) {
                for (u32 e = 0; e < elements; ++e) sampler_bindings.insert(binding + e);
            } else if (t->op == OpTypeImage && t->operands.size() > 5 && t->operands[5] == 2) {
                for (u32 e = 0; e < elements; ++e) {
                    (writes ? rw_texture_bindings : storage_texture_bindings).insert(binding + e);
                }
            }
        } else if (variable.storage == StorageBuffer ||
                   (variable.storage == Uniform && decoration(pointee, BufferBlock))) {
            (writes ? rw_buffer_bindings : storage_buffer_bindings).insert(binding);
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
    layout.readwrite_storage_textures = static_cast<u32>(rw_texture_bindings.size());
    layout.readwrite_storage_buffers = static_cast<u32>(rw_buffer_bindings.size());
    std::copy(std::begin(layout_local_size), std::end(layout_local_size), layout.local_size);
    return layout;
}

} // namespace kin
