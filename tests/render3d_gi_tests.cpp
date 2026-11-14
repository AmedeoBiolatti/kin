// Raw GI resolve checks use controlled probe fields so beauty shading cannot hide leakage.
#include "../engine/src/renderer/gpu/gpu_device.hpp"
#include "gi_test_shaders.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <kin/platform/app.hpp>
#include <kin/render3d/camera3d.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <kin/renderer/backend.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

using namespace kin;
namespace {
bool gpu_ready = false;
void need(bool value, const std::string& message) {
    if (!value)
        throw std::runtime_error(message);
}
void complete(SDL_GPUDevice* device, SDL_GPUCommandBuffer* cmd) {
    auto* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    need(fence && SDL_WaitForGPUFences(device, true, &fence, 1), "GPU submission/fence failed");
    SDL_ReleaseGPUFence(device, fence);
}
struct Probes {
    float counts[4]{}, bias[4]{}, level[4][4]{}, centre[4]{};
};
struct Params {
    float to_world[16]{}, unproject_xy[4]{}, unproject_zw[4]{}, sizes[4]{}, eye[4]{}, forward[4]{};
    Probes probes;
    float previous[16]{}, previous_eye[4]{}, previous_forward[4]{};
};
static_assert(sizeof(Params) == 352);
float half(u16 v) {
    const int e = (v >> 10) & 31;
    if (e == 31)
        return std::numeric_limits<float>::infinity();
    return std::ldexp(e ? 1.f + (v & 1023) / 1024.f : (v & 1023) / 1024.f, e ? e - 15 : -14) *
           ((v & 0x8000) ? -1.f : 1.f);
}
void upload_texture(gpu::GpuDevice& device, gpu::GpuTexture& texture, int w, int h, int layers, const void* data,
                    u32 bytes) {
    auto* d = device.handle();
    SDL_GPUTransferBufferCreateInfo info{.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = bytes};
    auto* transfer = SDL_CreateGPUTransferBuffer(d, &info);
    need(transfer, "upload buffer unavailable");
    auto* mapped = SDL_MapGPUTransferBuffer(d, transfer, false);
    need(mapped, "upload map failed");
    std::memcpy(mapped, data, bytes);
    SDL_UnmapGPUTransferBuffer(d, transfer);
    auto* cmd = SDL_AcquireGPUCommandBuffer(d);
    need(cmd, "upload command failed");
    auto* pass = SDL_BeginGPUCopyPass(cmd);
    need(pass, "upload pass failed");
    SDL_GPUTextureTransferInfo source{.transfer_buffer = transfer, .pixels_per_row = u32(w), .rows_per_layer = u32(h)};
    SDL_GPUTextureRegion target{.texture = texture.handle(), .w = u32(w), .h = u32(h), .d = u32(layers)};
    SDL_UploadToGPUTexture(pass, &source, &target, false);
    SDL_EndGPUCopyPass(pass);
    complete(d, cmd);
    SDL_ReleaseGPUTransferBuffer(d, transfer);
}
struct Fixture {
    gpu::GpuDevice& device;
    SDL_GPUDevice* d;
    int width, height, hw, hh, coverage_channels = 2;
    std::vector<gpu::GpuTexture> inputs;
    gpu::GpuTexture light, slope, coverage;
    SDL_GPUSampler* sampler = nullptr;
    SDL_GPUComputePipeline* pipeline = nullptr;
    std::vector<float> depth, normals, previous_normals, previous_light, probe_data;
    Params params{};
    Fixture(gpu::GpuDevice& dev, int w, int h)
        : device(dev), d(dev.handle()), width(w), height(h), hw((w + 1) / 2), hh((h + 1) / 2) {
        constexpr auto use = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
        inputs.push_back(dev.create_target(w, h, SDL_GPU_TEXTUREFORMAT_R32_FLOAT, use));
        inputs.push_back(dev.create_target(w, h, SDL_GPU_TEXTUREFORMAT_R32G32_FLOAT, use));
        inputs.push_back(dev.create_volume(32, 32, 4, SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT, use));
        inputs.push_back(dev.create_volume(64, 64, 4, SDL_GPU_TEXTUREFORMAT_R32G32_FLOAT, use));
        inputs.push_back(dev.create_volume(4, 4, 4, SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT, use));
        inputs.push_back(dev.create_target(hw, hh, SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT, use));
        inputs.push_back(dev.create_target(hw, hh, SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT, use));
        inputs.push_back(dev.create_target(w, h, SDL_GPU_TEXTUREFORMAT_R32G32_FLOAT, use));
        light = dev.create_target(hw, hh, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, use);
        slope = dev.create_target(hw, hh, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, use);
        coverage = dev.create_target(hw, hh, SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT, use);
        SDL_GPUSamplerCreateInfo sample{};
        sample.min_filter = sample.mag_filter = SDL_GPU_FILTER_LINEAR;
        sample.address_mode_u = sample.address_mode_v = sample.address_mode_w =
            SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        sampler = SDL_CreateGPUSampler(d, &sample);
        auto code = gi_test_shaders("gi_resolve.comp.spv");
        SDL_GPUComputePipelineCreateInfo info{};
        info.code = code.data();
        info.code_size = code.size();
        info.entrypoint = "main";
        info.format = SDL_GPU_SHADERFORMAT_SPIRV;
        info.num_samplers = 8;
        info.num_readwrite_storage_textures = 3;
        info.num_uniform_buffers = 1;
        info.threadcount_x = info.threadcount_y = 8;
        info.threadcount_z = 1;
        pipeline = SDL_CreateGPUComputePipeline(d, &info);
        need(sampler && pipeline, "resolve pipeline unavailable");
        auto camera = Camera3D::look_at({0, 0, 0}, {0, 0, -1});
        camera.projection = Projection3D::Orthographic;
        camera.height = 1;
        camera.z_near = .1f;
        camera.z_far = 10;
        const auto projection = camera.projection_matrix(float(w) / h), inv = projection.inverse();
        const auto world = camera.view().inverse(), vp = projection * camera.view();
        std::memcpy(params.to_world, world.m, 64);
        std::memcpy(params.previous, vp.m, 64);
        params.unproject_xy[0] = inv.at(0, 0);
        params.unproject_xy[1] = inv.at(1, 1);
        params.unproject_xy[2] = inv.at(0, 3);
        params.unproject_xy[3] = inv.at(1, 3);
        params.unproject_zw[0] = inv.at(2, 2);
        params.unproject_zw[1] = inv.at(2, 3);
        params.unproject_zw[2] = inv.at(3, 2);
        params.unproject_zw[3] = inv.at(3, 3);
        params.sizes[0] = hw;
        params.sizes[1] = hh;
        params.sizes[2] = w;
        params.sizes[3] = h;
        params.eye[3] = params.previous_eye[3] = 1;
        params.forward[2] = params.previous_forward[2] = -1;
        for (int i = 0; i < 3; ++i) {
            params.probes.counts[i] = 4;
            params.probes.level[0][i] = -2;
        }
        params.probes.counts[3] = 1;
        params.probes.bias[0] = .3f;
        params.probes.bias[1] = .1f;
        params.probes.bias[2] = params.probes.bias[3] = params.probes.level[0][3] = 1;
        params.probes.centre[3] = -1;
        depth.assign(w * h, projection.apply_projective({0, 0, -1}).z);
        normals.assign(w * h * 2, 0);
        previous_normals = normals;
        previous_light.assign(hw * hh * 4, 3);
        for (int i = 0; i < hw * hh; ++i)
            previous_light[i * 4 + 3] = 1;
        probe_data.assign(4 * 4 * 4 * 4, 0);
        for (int i = 0; i < 64; ++i)
            probe_data[i * 4 + 3] = 1;
        std::vector<float> ir(32 * 32 * 4 * 4, 1), moments(64 * 64 * 4 * 2), sl(hw * hh * 4, 0);
        for (size_t i = 0; i < moments.size(); i += 2) {
            moments[i] = 100;
            moments[i + 1] = 10000;
        }
        for (int i = 0; i < hw * hh; ++i)
            sl[i * 4 + 3] = 1;
        upload(2, 32, 32, 4, ir);
        upload(3, 64, 64, 4, moments);
        upload(6, hw, hh, 1, sl);
    }
    ~Fixture() {
        if (pipeline)
            SDL_ReleaseGPUComputePipeline(d, pipeline);
        if (sampler)
            SDL_ReleaseGPUSampler(d, sampler);
    }
    void upload(int input, int w, int h, int layers, const std::vector<float>& values) {
        upload_texture(device, inputs[input], w, h, layers, values.data(), u32(values.size() * sizeof(float)));
    }
    void old_normal(int x, int y, float nx, float ny) {
        previous_normals[(y * width + x) * 2] = nx;
        previous_normals[(y * width + x) * 2 + 1] = ny;
    }
    std::array<float, 4> run(int x = 1, int y = 1) {
        upload(0, width, height, 1, depth);
        upload(1, width, height, 1, normals);
        upload(4, 4, 4, 4, probe_data);
        upload(5, hw, hh, 1, previous_light);
        upload(7, width, height, 1, previous_normals);
        auto* cmd = SDL_AcquireGPUCommandBuffer(d);
        need(cmd, "resolve command failed");
        const std::array<SDL_GPUStorageTextureReadWriteBinding, 3> targets{
            {{.texture = light.handle()}, {.texture = slope.handle()}, {.texture = coverage.handle()}}};
        auto* pass = SDL_BeginGPUComputePass(cmd, targets.data(), 3, nullptr, 0);
        need(pass, "resolve pass failed");
        std::vector<SDL_GPUTextureSamplerBinding> samples;
        for (auto& input : inputs)
            samples.push_back({input.handle(), sampler});
        SDL_BindGPUComputePipeline(pass, pipeline);
        SDL_BindGPUComputeSamplers(pass, 0, samples.data(), 8);
        SDL_PushGPUComputeUniformData(cmd, 0, &params, sizeof(params));
        SDL_DispatchGPUCompute(pass, (hw + 7) / 8, (hh + 7) / 8, 1);
        SDL_EndGPUComputePass(pass);
        complete(d, cmd);
        return read(x, y);
    }
    std::array<float, 4> read(int x, int y) {
        SDL_GPUTransferBufferCreateInfo info{.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, .size = u32(hw * hh * 8)};
        auto* transfer = SDL_CreateGPUTransferBuffer(d, &info);
        need(transfer, "download buffer unavailable");
        auto* cmd = SDL_AcquireGPUCommandBuffer(d);
        auto* copy = SDL_BeginGPUCopyPass(cmd);
        need(copy, "download pass failed");
        SDL_GPUTextureRegion source{.texture = light.handle(), .w = u32(hw), .h = u32(hh), .d = 1};
        SDL_GPUTextureTransferInfo target{
            .transfer_buffer = transfer, .pixels_per_row = u32(hw), .rows_per_layer = u32(hh)};
        SDL_DownloadFromGPUTexture(copy, &source, &target);
        SDL_EndGPUCopyPass(copy);
        complete(d, cmd);
        auto* bytes = static_cast<const u16*>(SDL_MapGPUTransferBuffer(d, transfer, false));
        need(bytes, "download map failed");
        std::array<float, 4> value{};
        for (int c = 0; c < 4; ++c)
            value[c] = half(bytes[(y * hw + x) * 4 + c]);
        SDL_UnmapGPUTransferBuffer(d, transfer);
        SDL_ReleaseGPUTransferBuffer(d, transfer);
        for (float v : value)
            need(std::isfinite(v), "nonfinite resolve output");
        return value;
    }
};
} // namespace

namespace {
float coverage_at(Fixture& f, int x = 1, int y = 1, int channel = 0) {
    SDL_GPUTransferBufferCreateInfo info{.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,
                                         .size = u32(f.hw * f.hh * f.coverage_channels * 2)};
    auto* transfer = SDL_CreateGPUTransferBuffer(f.d, &info);
    need(transfer, "coverage buffer unavailable");
    auto* cmd = SDL_AcquireGPUCommandBuffer(f.d);
    auto* pass = SDL_BeginGPUCopyPass(cmd);
    need(pass, "coverage copy unavailable");
    SDL_GPUTextureRegion source{.texture = f.coverage.handle(), .w = u32(f.hw), .h = u32(f.hh), .d = 1};
    SDL_GPUTextureTransferInfo target{
        .transfer_buffer = transfer, .pixels_per_row = u32(f.hw), .rows_per_layer = u32(f.hh)};
    SDL_DownloadFromGPUTexture(pass, &source, &target);
    SDL_EndGPUCopyPass(pass);
    complete(f.d, cmd);
    auto* bytes = static_cast<const u16*>(SDL_MapGPUTransferBuffer(f.d, transfer, false));
    need(bytes, "coverage map unavailable");
    const float result = half(bytes[(y * f.hw + x) * f.coverage_channels + channel]);
    SDL_UnmapGPUTransferBuffer(f.d, transfer);
    SDL_ReleaseGPUTransferBuffer(f.d, transfer);
    return result;
}

void resolve_contracts(Window& window) {
    gpu::GpuDevice device(window);
    gpu_ready = true;
    for (const std::string name : {"visible", "blocked", "dark", "missing", "background"}) {
        Fixture f(device, 8, 8);
        f.params.previous_eye[3] = 0;
        float expected = 1, coverage = 1, support = 1;
        if (name == "blocked") {
            // Bright probes whose moment field completely occludes the receiver must not normalize to full light, and
            // give up seven tenths of their share (the caller's ambient takes that: dim, not black).
            f.upload(3, 64, 64, 4, std::vector<float>(64 * 64 * 4 * 2, 0));
            expected = 0;
            coverage = 0.3f;
        } else if (name == "dark") {
            f.upload(2, 32, 32, 4, std::vector<float>(32 * 32 * 4 * 4, 0));
            expected = 0;
        } else if (name == "missing") {
            for (size_t i = 3; i < f.probe_data.size(); i += 4)
                f.probe_data[i] = 0;
            expected = coverage = support = 0;
        } else if (name == "background") {
            std::fill(f.depth.begin(), f.depth.end(), 0);
            expected = coverage = support = 0;
        }
        const auto light = f.run();
        for (int c = 0; c < 3; ++c)
            need(std::abs(light[c] - expected) < .001f, name + ": wrong raw GI");
        need(std::abs(coverage_at(f) - coverage) < .001f, name + ": wrong coverage");
        need(std::abs(coverage_at(f, 1, 1, 1) - support) < .001f, name + ": wrong domain/readiness");
        std::cout << "GI resolve " << name << ": light " << light[0] << ", coverage " << coverage << '\n';
    }
    {
        // The receiver lies on the z=-1 probe plane. The row nearer an occluder is darker: changing only the
        // eye offset must not move interpolation into that row. The normal offset still selects free-side probes.
        Fixture f(device, 8, 8);
        f.params.previous_eye[3] = 0;
        std::vector<float> ir(32 * 32 * 4 * 4, .1f);
        for (int z = 0; z < 4; ++z)
            for (int y = 0; y < 32; ++y)
                for (int x = 0; x < 32; ++x)
                    for (int c = 0; c < 4; ++c)
                        ir[((z * 32 + y) * 32 + x) * 4 + c] = z == 3 ? .6f : .1f; // wrapped z=-1
        f.upload(2, 32, 32, 4, ir);
        for (const float normal_bias : {0.0f, .3f}) {
            f.params.probes.bias[0] = normal_bias;
            f.params.probes.bias[1] = 0;
            const float reference = f.run()[0];
            need(reference > .3f, "receiver lost its visible probe light");
            for (const float eye_bias : {.1f, .2f}) {
                f.params.probes.bias[1] = eye_bias;
                const auto light = f.run();
                std::cout << "GI receiver interpolation bias " << normal_bias << ',' << eye_bias << ": " << light[0]
                          << '\n';
                need(std::abs(light[0] - reference) < .001f, "eye bias displaced receiver interpolation");
            }
        }
        // A probe plane inside a wall must not leave its receiver black. The free-side cell still supplies the
        // light when all corners on the receiver's exact plane are excluded by probe placement.
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x)
                f.probe_data[((3 * 4 + y) * 4 + x) * 4 + 3] = 0;
        need(std::abs(f.run()[0] - .1f) < .001f, "excluded probe plane lost free-side lighting");
        for (int i = 0; i < 64; ++i)
            f.probe_data[i * 4 + 3] = 1;
        // The same bias remains part of the moment test. These probes see the unbiased receiver but not its
        // displaced endpoint, so disabling visibility bias along with the interpolation shift would leak light.
        std::vector<float> moments(64 * 64 * 4 * 2);
        for (size_t i = 0; i < moments.size(); i += 2) {
            moments[i] = .3f;
            moments[i + 1] = .09f;
        }
        f.upload(3, 64, 64, 4, moments);
        f.params.probes.bias[0] = f.params.probes.bias[1] = 0;
        need(f.run()[0] > .59f, "visible receiver lost its probe light");
        for (const auto bias : {Vec2f{.3f, .1f}, Vec2f{0, .4f}}) {
            f.params.probes.bias[0] = bias.x;
            f.params.probes.bias[1] = bias.y;
            need(f.run()[0] < .001f, "receiver interpolation removed biased visibility testing");
        }
    }
}

// The production placement shader must retain free nominal cells and move out through a surface's front.
void placement_contracts(Window& window) {
    gpu::GpuDevice device(window);
    gpu_ready = true;
    auto* d = device.handle();
    constexpr auto sampled = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    constexpr auto writable = sampled | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE |
                              SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE;
    auto distance = device.create_volume(8, 8, 8, SDL_GPU_TEXTUREFORMAT_R32_FLOAT, sampled);
    auto normals = device.create_volume(8, 8, 8, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, sampled);
    auto probes = device.create_volume(2, 2, 2, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, writable);
    auto irradiance = device.create_volume(16, 16, 2, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, writable);
    auto moments = device.create_volume(32, 32, 2, SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT, writable);
    const u32 first = 1u << 23;
    auto list = device.create_buffer(SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ, &first, sizeof(first));
    SDL_GPUSamplerCreateInfo sampling{};
    sampling.min_filter = sampling.mag_filter = SDL_GPU_FILTER_NEAREST;
    sampling.address_mode_u = sampling.address_mode_v = sampling.address_mode_w =
        SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    auto* sampler = SDL_CreateGPUSampler(d, &sampling);
    const auto code = gi_test_shaders("gi_probe_place.comp.spv");
    SDL_GPUComputePipelineCreateInfo info{};
    info.code = code.data();
    info.code_size = code.size();
    info.entrypoint = "main";
    info.format = SDL_GPU_SHADERFORMAT_SPIRV;
    info.num_samplers = 2;
    info.num_readonly_storage_buffers = 1;
    info.num_readwrite_storage_textures = 3;
    info.num_uniform_buffers = 1;
    info.threadcount_x = 64;
    info.threadcount_y = info.threadcount_z = 1;
    auto* pipeline = SDL_CreateGPUComputePipeline(d, &info);
    need(sampler && pipeline, "probe placement pipeline unavailable");
    struct PlaceParams {
        float size[4]{8, 8, 8, 1}, level[4][4]{{-4, -4, -4, 1}};
        Probes probes;
        u32 count[4]{1, 0, 0, 0};
        float misc[4]{0, 7.9f, 0, 0};
    } params;
    static_assert(sizeof(PlaceParams) == 224);
    params.probes.counts[0] = params.probes.counts[1] = params.probes.counts[2] = 2;
    params.probes.level[0][3] = 4;
    const auto run = [&](int plane, bool positive, bool two_sided, bool fresh, bool blocked = false) {
        std::vector<float> distances(8 * 8 * 8);
        std::vector<u8> faces(8 * 8 * 8 * 4);
        for (int z = 0; z < 8; ++z)
            for (int y = 0; y < 8; ++y)
                for (int x = 0; x < 8; ++x) {
                    const int at = x + 8 * (y + 8 * z), world_x = x < 4 ? x : x - 8;
                    distances[at] = blocked ? 0.f : float(std::abs(world_x - plane));
                    faces[at * 4] = positive ? 255 : 0;
                    faces[at * 4 + 1] = faces[at * 4 + 2] = 128;
                    faces[at * 4 + 3] = two_sided ? 255 : 128;
                }
        upload_texture(device, distance, 8, 8, 8, distances.data(), u32(distances.size() * sizeof(float)));
        upload_texture(device, normals, 8, 8, 8, faces.data(), u32(faces.size()));
        if (fresh) {
            const std::vector<u16> zeros(2 * 2 * 2 * 4, 0), lit(16 * 16 * 2 * 4, 0x3c00), old(32 * 32 * 2 * 2, 0x3c00);
            upload_texture(device, probes, 2, 2, 2, zeros.data(), u32(zeros.size() * sizeof(u16)));
            upload_texture(device, irradiance, 16, 16, 2, lit.data(), u32(lit.size() * sizeof(u16)));
            upload_texture(device, moments, 32, 32, 2, old.data(), u32(old.size() * sizeof(u16)));
        }
        const u32 entry = fresh ? 1u << 23 : 0;
        device.upload_buffer(list, &entry, sizeof(entry));
        auto* commands = SDL_AcquireGPUCommandBuffer(d);
        const std::array<SDL_GPUStorageTextureReadWriteBinding, 3> outputs{
            SDL_GPUStorageTextureReadWriteBinding{.texture = probes.handle()},
            SDL_GPUStorageTextureReadWriteBinding{.texture = irradiance.handle()},
            SDL_GPUStorageTextureReadWriteBinding{.texture = moments.handle()}};
        auto* pass = SDL_BeginGPUComputePass(commands, outputs.data(), 3, nullptr, 0);
        need(pass, "probe placement pass unavailable");
        const std::array<SDL_GPUTextureSamplerBinding, 2> inputs{
            SDL_GPUTextureSamplerBinding{distance.handle(), sampler},
            SDL_GPUTextureSamplerBinding{normals.handle(), sampler}};
        SDL_GPUBuffer* input = list.handle();
        SDL_BindGPUComputePipeline(pass, pipeline);
        SDL_BindGPUComputeSamplers(pass, 0, inputs.data(), 2);
        SDL_BindGPUComputeStorageBuffers(pass, 0, &input, 1);
        SDL_PushGPUComputeUniformData(commands, 0, &params, sizeof(params));
        SDL_DispatchGPUCompute(pass, 1, 1, 1);
        SDL_EndGPUComputePass(pass);
        complete(d, commands);
        std::vector<u8> bytes;
        need(device.read_volume(probes, 2, 8, bytes), "cannot read placed probe");
        std::array<u16, 4> encoded;
        std::memcpy(encoded.data(), bytes.data(), sizeof(encoded));
        return std::array<float, 4>{half(encoded[0]), half(encoded[1]), half(encoded[2]), half(encoded[3])};
    };
    const auto adjacent = run(2, false, false, true);
    need(adjacent[0] == 0 && adjacent[1] == 0 && adjacent[2] == 0 && adjacent[3] == .25f,
         "already-clear nominal probe was displaced or became ready before tracing");
    std::vector<u8> bytes;
    need(device.read_volume(irradiance, 2, 8, bytes), "cannot read placed probe history");
    u16 fresh_alpha = 1, other_alpha = 0;
    std::memcpy(&fresh_alpha, bytes.data() + ((1 + 16) * 4 + 3) * 2, 2);
    std::memcpy(&other_alpha, bytes.data() + ((9 + 16 * 9) * 4 + 3) * 2, 2);
    need(fresh_alpha == 0 && other_alpha == 0x3c00, "placing a new probe did not clear only its own history");
    const auto outward = run(0, true, false, true);
    need(outward[0] > 1 && outward[0] < 2 && outward[1] == 0 && outward[2] == 0 && outward[3] == .25f,
         "single-sided relocation crossed behind the surface instead of reaching a front empty cell");
    const auto negative = run(0, false, false, true);
    need(negative[0] < -1 && negative[0] > -2 && negative[1] == 0 && negative[2] == 0,
         "negative-facing surface did not choose its nearest front empty cell");
    const auto retained = run(2, true, false, false);
    need(retained == negative, "a kept free probe moved when geometry changed elsewhere");
    const auto both = run(0, true, true, true);
    need(both[0] < -1 && both[0] > -2, "double-sided surface lost its nearer valid side");
    const auto trapped = run(0, true, false, true, true);
    need(trapped[3] < 0, "probe with no free destination became usable");
    const std::vector<u16> inside(2 * 2 * 2 * 4, 0);
    auto marked = inside;
    marked[3] = 0xbc00;
    upload_texture(device, probes, 2, 2, 2, marked.data(), u32(marked.size() * sizeof(u16)));
    const auto still_inside = run(1, false, false, false);
    need(still_inside[3] < 0, "placement woke a probe classified inside before its rays proved it free");
    SDL_ReleaseGPUComputePipeline(d, pipeline);
    SDL_ReleaseGPUSampler(d, sampler);
    std::cout << "GI placement: clear probes retained; front, double-sided, trapped and history contracts passed\n";
}

// Exercise the production probe-ray shader without beauty shading or probe integration hiding a false hit.
void surface_hit_contracts(Window& window) {
    gpu::GpuDevice device(window);
    gpu_ready = true;
    auto* d = device.handle();
    struct TraceParams {
        float size[4]{}, level[4][4]{};
        Probes probes;
        u32 batch[4]{512, 1, 1, 0};
        float rotation[16]{}, sky[4]{}, environment[16]{}, ambient[4]{.25f, .25f, .25f, 0},
            ground[4]{.25f, .25f, .25f, 0};
        struct Portal {
            float entry[16]{}, through[16]{}, opening[4]{};
        } portals[8];
        u32 portal_count[4]{};
    } params;
    static_assert(sizeof(TraceParams) == 1552);
    constexpr auto use = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    std::vector<gpu::GpuTexture> inputs;
    for (int i = 0; i < 9; ++i) {
        if (i == 5)
            inputs.push_back(device.create_target(1, 1, SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT, use));
        else if (i == 4)
            inputs.push_back(device.create_volume(2, 2, 2, SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT, use));
        else
            inputs.push_back(device.create_volume(8, 8, 16,
                                                  i == 0   ? SDL_GPU_TEXTUREFORMAT_R32_FLOAT
                                                  : i == 8 ? SDL_GPU_TEXTUREFORMAT_R8_UNORM
                                                           : SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT,
                                                  use));
    }
    const u32 zero = 0;
    auto list = device.create_buffer(SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ, &zero, sizeof(zero));
    auto empty = device.create_buffer(SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ, &zero, sizeof(zero));
    auto rays = device.create_buffer(SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE, nullptr, 16);
    SDL_GPUSamplerCreateInfo sampling{};
    sampling.min_filter = sampling.mag_filter = SDL_GPU_FILTER_NEAREST;
    sampling.address_mode_u = sampling.address_mode_v = sampling.address_mode_w =
        SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    auto* sampler = SDL_CreateGPUSampler(d, &sampling);
    const auto code = gi_test_shaders("gi_probe_trace.comp.spv");
    SDL_GPUComputePipelineCreateInfo pipeline_info{};
    pipeline_info.code = code.data();
    pipeline_info.code_size = code.size();
    pipeline_info.entrypoint = "main";
    pipeline_info.format = SDL_GPU_SHADERFORMAT_SPIRV;
    pipeline_info.num_samplers = 9;
    pipeline_info.num_readonly_storage_buffers = 3;
    pipeline_info.num_readwrite_storage_buffers = 1;
    pipeline_info.num_uniform_buffers = 1;
    pipeline_info.threadcount_x = 64;
    pipeline_info.threadcount_y = pipeline_info.threadcount_z = 1;
    auto* pipeline = SDL_CreateGPUComputePipeline(d, &pipeline_info);
    need(sampler && pipeline, "probe trace pipeline unavailable");
    const auto run = [&](Vec3f first, Vec3f point, Vec3f direction, u8 mask, int levels = 1, bool two_sided = false) {
        std::vector<float> distance(8 * 8 * 16, 1), normal(8 * 8 * 16 * 4, .5f), radiance(normal.size(), 0),
            back(normal.size(), 0), probes(2 * 2 * 2 * 4, 0), blank(normal.size(), 0);
        std::vector<u8> coverage(8 * 8 * 16, 0);
        const auto index = [&](int x, int y, int z, int level = 0) {
            const auto wrap = [](int a) { return (a % 8 + 8) % 8; };
            return wrap(x + int(first.x)) + 8 * (wrap(y + int(first.y)) + 8 * (wrap(z + int(first.z)) + 8 * level));
        };
        for (int y = 0; y < 8; ++y)
            for (int z = 0; z < 8; ++z) {
                const int i = index(5, y, z, levels - 1);
                distance[i] = 0;
                coverage[i] = 255;
                normal[i * 4] = 0; // a real wall facing -x
                normal[i * 4 + 3] = two_sided ? 1 : .5f;
                radiance[i * 4 + 1] = 1;
            }
        const int partial = index(2, 0, 0);
        distance[partial] = 0;
        coverage[partial] = mask;
        normal[partial * 4 + 2] = 1; // the column's stored normal faces +z
        radiance[partial * 4] = 1;
        for (int i = 0; i < 8 * 8 * 16; ++i)
            back[i * 4 + 2] = 1;
        const Vec3f at = first + point;
        probes[0] = at.x;
        probes[1] = at.y;
        probes[2] = at.z;
        probes[3] = 1;
        std::fill(std::begin(params.size), std::end(params.size), 8);
        params.size[3] = float(levels);
        for (int k = 0; k < levels; ++k) {
            params.level[k][0] = first.x;
            params.level[k][1] = first.y;
            params.level[k][2] = first.z;
            params.level[k][3] = float(1 << k);
        }
        for (int a = 0; a < 3; ++a)
            params.probes.counts[a] = 2;
        params.probes.level[0][3] = 1;
        const Vec3f way = normalize(direction);
        params.rotation[0] = way.x;
        params.rotation[1] = way.y;
        params.rotation[2] = way.z; // the single test ray's Fibonacci direction is +x
        const auto upload = [&](int i, const std::vector<float>& values) {
            upload_texture(device, inputs[i], i == 4 ? 2 : 8, i == 4 ? 2 : 8, i == 4 ? 2 : 16, values.data(),
                           u32(values.size() * sizeof(float)));
        };
        upload(0, distance);
        upload(1, blank);
        upload(2, normal);
        upload(3, radiance);
        upload(4, probes);
        upload(6, back);
        upload(7, blank);
        const std::array<float, 4> sky{};
        upload_texture(device, inputs[5], 1, 1, 1, sky.data(), sizeof(sky));
        upload_texture(device, inputs[8], 8, 8, 16, coverage.data(), u32(coverage.size()));
        auto* cmd = SDL_AcquireGPUCommandBuffer(d);
        const SDL_GPUStorageBufferReadWriteBinding output{.buffer = rays.handle()};
        auto* pass = SDL_BeginGPUComputePass(cmd, nullptr, 0, &output, 1);
        need(pass, "probe trace pass failed");
        std::vector<SDL_GPUTextureSamplerBinding> samplers;
        for (auto& input : inputs)
            samplers.push_back({input.handle(), sampler});
        const std::array<SDL_GPUBuffer*, 3> buffers{list.handle(), empty.handle(), empty.handle()};
        SDL_BindGPUComputePipeline(pass, pipeline);
        SDL_BindGPUComputeSamplers(pass, 0, samplers.data(), u32(samplers.size()));
        SDL_BindGPUComputeStorageBuffers(pass, 0, buffers.data(), u32(buffers.size()));
        SDL_PushGPUComputeUniformData(cmd, 0, &params, sizeof(params));
        SDL_DispatchGPUCompute(pass, 1, 1, 1);
        SDL_EndGPUComputePass(pass);
        complete(d, cmd);
        std::vector<u8> bytes;
        need(device.read_buffer(rays.handle(), 0, 16, bytes), "cannot read probe ray");
        std::array<float, 4> value;
        std::memcpy(value.data(), bytes.data(), sizeof(value));
        return value;
    };
    for (const auto first : {Vec3f{0, 0, 0}, Vec3f{-5, 0, -3}}) {
        const auto coarse = run(first, {.5f, .25f, .65f}, {1, 0, .2f}, 255);
        need(coarse[0] == 0 && coarse[1] == 0 && coarse[3] < 0, "coarse false-backface control changed");
        const auto clear = run(first, {.5f, .25f, .65f}, {1, 0, .2f}, 15);
        need(clear[1] > .99f && clear[3] > 4, "empty part of occupied voxel still blocks the bright wall");
        const auto unknown = run(first, {.5f, .25f, .65f}, {1, 0, .2f}, 0);
        need(unknown[3] < 0, "unknown coverage opened a surface");
        const auto later = run(first, {.1f, .25f, .25f}, {1, 0, .01f}, 0xaa);
        need(later[0] == 0 && std::abs(later[3] + 1.9f) < .001f, "ray lost a conservative back in its covered half");
        const auto front = run(first, {.5f, .25f, .65f}, {1, 0, -.01f}, 15);
        need(front[0] > .99f, "front-facing coarse estimator changed");
        const auto thin = run(first, {.5f, .25f, .4f}, {1, 0, -.1f}, 15);
        need(thin[0] > .99f, "conservative thin-surface coverage leaked");
        const auto blocked = run(first, {6.5f, .25f, .25f}, {-1, 0, 0}, 15);
        need(blocked[3] < 0 && blocked[2] == 0, "real one-sided backface gained light");
        const auto both = run(first, {6.5f, .25f, .25f}, {-1, 0, 0}, 15, 1, true);
        need(both[2] > .99f && both[3] > 0, "double-sided surface lost its back's light");
    }
    const auto outer = run({}, {.5f, .25f, .65f}, {1, 0, .2f}, 15, 2);
    need(outer[1] > .99f && outer[3] > 9, "refined ray did not continue into the outer window");
    SDL_ReleaseGPUComputePipeline(d, pipeline);
    SDL_ReleaseGPUSampler(d, sampler);
    std::cout << "GI surface hits: false hits rejected; thin, back, double-sided and wrapped/coarser hits retained\n";
}

void surface_reconstruction_contracts(Window& window) {
    gpu::GpuDevice device(window);
    gpu_ready = true;
    for (const std::string name :
         {"plane_hole", "wide_hole", "perspective", "dark_neighbours", "window_fade", "outside_window", "untraced",
          "normal_boundary", "depth_boundary", "disconnected", "world_bound", "legitimate_dark", "no_donor",
          "partial_confidence", "weak_visibility", "odd_edge"}) {
        Fixture f(device, 65, 49);
        const int cx = name == "odd_edge" ? f.hw - 1 : f.hw / 2;
        const int cy = name == "odd_edge" ? f.hh - 1 : f.hh / 2;
        const int hole_radius = name == "wide_hole" ? 6 : 2;
        const auto hole = [&](int x, int y) {
            return std::abs(x - cx) <= hole_radius && std::abs(y - cy) <= hole_radius;
        };
        if (name == "perspective") {
            auto camera = Camera3D::look_at({}, {0, 0, -1}, 90);
            camera.z_near = .1f;
            camera.z_far = 10;
            const auto projection = camera.projection_matrix(float(f.width) / f.height), inverse = projection.inverse();
            f.params.unproject_xy[0] = inverse.at(0, 0);
            f.params.unproject_xy[1] = inverse.at(1, 1);
            f.params.unproject_xy[2] = inverse.at(0, 3);
            f.params.unproject_xy[3] = inverse.at(1, 3);
            f.params.unproject_zw[0] = inverse.at(2, 2);
            f.params.unproject_zw[1] = inverse.at(2, 3);
            f.params.unproject_zw[2] = inverse.at(3, 2);
            f.params.unproject_zw[3] = inverse.at(3, 3);
            std::fill(f.depth.begin(), f.depth.end(), projection.apply_projective({0, 0, -1}).z);
        }
        std::vector<float> raw(f.hw * f.hh * 4), slope(raw.size(), 0), shares(f.hw * f.hh * 2, 1);
        for (int y = 0; y < f.hh; ++y) {
            for (int x = 0; x < f.hw; ++x) {
                const int i = y * f.hw + x;
                const bool missing = hole(x, y);
                raw[i * 4 + 3] = slope[i * 4 + 3] = 1;
                for (int c = 0; c < 3; ++c)
                    raw[i * 4 + c] = missing || name == "dark_neighbours" ? 0 : .04f * (c + 1);
                if (missing || name == "no_donor")
                    shares[i * 2] = .3f;
                if (missing && name == "window_fade") {
                    shares[i * 2] = .12f;
                    shares[i * 2 + 1] = .4f;
                }
                if (missing && (name == "outside_window" || name == "untraced"))
                    shares[i * 2] = shares[i * 2 + 1] = 0;
                if (missing && name == "legitimate_dark")
                    shares[i * 2] = 1;
                if (missing && name == "partial_confidence")
                    shares[i * 2] = .92f;
                if (missing && name == "weak_visibility")
                    shares[i * 2] = .99f;
                for (int dy = 0; dy < 2; ++dy) {
                    for (int dx = 0; dx < 2; ++dx) {
                        const int px = std::min(x * 2 + dx, f.width - 1), py = std::min(y * 2 + dy, f.height - 1);
                        const int pixel = py * f.width + px;
                        if (!missing && name == "normal_boundary")
                            f.normals[pixel * 2] = 1;
                        if (!missing && name == "depth_boundary")
                            f.depth[pixel] = (f.params.unproject_zw[1] + 2 * f.params.unproject_zw[3]) /
                                             (-2 * f.params.unproject_zw[2] - f.params.unproject_zw[0]);
                        const int distance = std::max(std::abs(x - cx), std::abs(y - cy));
                        if (name == "disconnected" && distance >= 3 && distance <= 5)
                            f.depth[pixel] = 0;
                    }
                }
            }
        }
        constexpr auto usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
        f.inputs.push_back(device.create_target(f.hw, f.hh, SDL_GPU_TEXTUREFORMAT_R32G32_FLOAT, usage));
        f.upload(0, f.width, f.height, 1, f.depth);
        f.upload(1, f.width, f.height, 1, f.normals);
        f.upload(5, f.hw, f.hh, 1, raw);
        f.upload(6, f.hw, f.hh, 1, slope);
        f.upload(8, f.hw, f.hh, 1, shares);
        f.coverage = device.create_target(f.hw, f.hh, SDL_GPU_TEXTUREFORMAT_R16_FLOAT, usage);
        f.coverage_channels = 1;
        SDL_ReleaseGPUComputePipeline(f.d, f.pipeline);
        f.pipeline = nullptr;
        const auto code = gi_test_shaders("gi_surface.comp.spv");
        SDL_GPUComputePipelineCreateInfo info{};
        info.code = code.data();
        info.code_size = code.size();
        info.entrypoint = "main";
        info.format = SDL_GPU_SHADERFORMAT_SPIRV;
        info.num_samplers = 5;
        info.num_readwrite_storage_textures = 3;
        info.num_uniform_buffers = 1;
        info.threadcount_x = info.threadcount_y = 8;
        info.threadcount_z = 1;
        f.pipeline = SDL_CreateGPUComputePipeline(f.d, &info);
        need(f.pipeline, "surface reconstruction pipeline unavailable");
        struct SurfaceParams {
            float to_world[16], xy[4], zw[4], sizes[4], repair[4];
        } params{};
        static_assert(sizeof(params) == 128);
        std::memcpy(&params, &f.params, 112);
        params.repair[0] = name == "world_bound" ? .01f : 2;
        params.repair[1] = .005f;
        auto* cmd = SDL_AcquireGPUCommandBuffer(f.d);
        const std::array<SDL_GPUStorageTextureReadWriteBinding, 3> targets{
            {{.texture = f.light.handle()}, {.texture = f.slope.handle()}, {.texture = f.coverage.handle()}}};
        auto* pass = SDL_BeginGPUComputePass(cmd, targets.data(), 3, nullptr, 0);
        need(pass, "surface reconstruction pass unavailable");
        const std::array<SDL_GPUTextureSamplerBinding, 5> samplers{{{f.inputs[0].handle(), f.sampler},
                                                                    {f.inputs[1].handle(), f.sampler},
                                                                    {f.inputs[5].handle(), f.sampler},
                                                                    {f.inputs[6].handle(), f.sampler},
                                                                    {f.inputs[8].handle(), f.sampler}}};
        SDL_BindGPUComputePipeline(pass, f.pipeline);
        SDL_BindGPUComputeSamplers(pass, 0, samplers.data(), 5);
        SDL_PushGPUComputeUniformData(cmd, 0, &params, sizeof(params));
        SDL_DispatchGPUCompute(pass, (f.hw + 7) / 8, (f.hh + 7) / 8, 1);
        SDL_EndGPUComputePass(pass);
        complete(f.d, cmd);
        const auto result = f.read(cx, cy);
        const bool repaired = name == "plane_hole" || name == "wide_hole" || name == "perspective" ||
                              name == "dark_neighbours" || name == "window_fade" || name == "odd_edge";
        for (int c = 0; c < 3; ++c) {
            const float expected = repaired && name != "dark_neighbours" ? .04f * (c + 1) : 0;
            if (name == "partial_confidence")
                need(result[c] > .005f && result[c] < .04f * (c + 1), name + ": missing partial donor contribution");
            else
                need(std::abs(result[c] - expected) < .001f, name + ": borrowed incorrect surface lighting");
        }
        // Neither a failed neighbour search nor a partial repair may reintroduce the unoccluded sky. Only the
        // receiver's domain/readiness share controls replacement, including when its conservative light is zero.
        const float expected_coverage = shares[(cy * f.hw + cx) * 2 + 1];
        need(std::abs(coverage_at(f, cx, cy) - expected_coverage) < .001f,
             name + ": changed window/readiness coverage");
        need(std::abs(result[3] - 1) < .001f, name + ": changed receiver depth");
        std::cout << "GI surface reconstruction " << name << ": light " << result[0] << ", coverage "
                  << coverage_at(f, cx, cy) << '\n';
    }
}

void surface_history_contracts(Window& window) {
    gpu::GpuDevice device(window);
    gpu_ready = true;
    for (const std::string name :
         {"accept", "reject_normal", "partial_footprint", "mixed_receiver", "smooth", "motion_marker", "thin_receiver",
          "depth_reject", "offscreen", "camera_reprojection", "no_history", "background", "no_support", "odd_extent"}) {
        const bool odd = name == "odd_extent";
        Fixture f(device, odd ? 7 : 8, odd ? 5 : 8);
        float expected = 2.7f;
        int x = 1, y = 1;
        if (name == "reject_normal" || name == "partial_footprint") {
            for (int yy = 0; yy < f.height; ++yy)
                for (int xx = 0; xx < f.width; ++xx)
                    f.old_normal(xx, yy, 0, 1);
            expected = 1;
            if (name == "partial_footprint") {
                for (int yy = 2; yy < 4; ++yy)
                    for (int xx = 2; xx < 4; ++xx)
                        f.old_normal(xx, yy, 0, 0);
                for (int i = 0; i < f.hw * f.hh; ++i)
                    for (int c = 0; c < 3; ++c)
                        f.previous_light[i * 4 + c] = i == f.hw + 1 ? 3 : 9;
                expected = 2.7f;
            }
        } else if (name == "mixed_receiver") {
            // One differently facing pixel invalidates each half-resolution history estimate.
            for (int yy = 0; yy < f.height; yy += 2)
                for (int xx = 0; xx < f.width; xx += 2)
                    f.old_normal(xx, yy, 0, 1);
            expected = 1;
        } else if (name == "smooth") {
            for (int yy = 0; yy < f.height; ++yy)
                for (int xx = 0; xx < f.width; ++xx)
                    f.old_normal(xx, yy, ((xx + yy) % 3 - 1) * .08f, 0);
        } else if (name == "motion_marker") {
            for (int yy = 0; yy < f.height; ++yy)
                for (int xx = 0; xx < f.width; ++xx)
                    f.old_normal(xx, yy, 0, 4);
        } else if (name == "thin_receiver") {
            auto camera = Camera3D::look_at({0, 0, 0}, {0, 0, -1});
            camera.projection = Projection3D::Orthographic;
            camera.height = 1;
            camera.z_near = .1f;
            camera.z_far = 10;
            const auto projection = camera.projection_matrix(1);
            std::fill(f.depth.begin(), f.depth.end(), projection.apply_projective({0, 0, -1.05f}).z);
            f.depth[3 * f.width + 3] = projection.apply_projective({0, 0, -1}).z;
            f.normals[(3 * f.width + 3) * 2 + 1] = 1;
            for (int yy = 0; yy < f.height; ++yy)
                for (int xx = 0; xx < f.width; ++xx)
                    f.old_normal(xx, yy, 0, 1);
        } else if (name == "depth_reject") {
            for (int i = 0; i < f.hw * f.hh; ++i)
                f.previous_light[i * 4 + 3] = 10;
            expected = 1;
        } else if (name == "offscreen" || name == "camera_reprojection") {
            Mat4f previous;
            std::memcpy(previous.m, f.params.previous, sizeof(previous.m));
            previous.at(0, 3) += name == "offscreen" ? 4.f : .5f;
            std::memcpy(f.params.previous, previous.m, sizeof(previous.m));
            if (name == "offscreen") {
                expected = 1;
            } else {
                for (int yy = 0; yy < f.hh; ++yy)
                    for (int c = 0; c < 3; ++c)
                        f.previous_light[(yy * f.hw + 2) * 4 + c] = 7;
                // Shifted footprint: .25*3 + .75*7, blended .85 with current 1.
                expected = 5.25f;
            }
        } else if (name == "no_history") {
            f.params.previous_eye[3] = 0;
            expected = 1;
        } else if (name == "background") {
            std::fill(f.depth.begin(), f.depth.end(), 0);
            expected = 0;
        } else if (name == "no_support") {
            std::fill(f.probe_data.begin(), f.probe_data.end(), 0);
            expected = 0;
        } else if (odd) {
            x = f.hw - 1;
            y = f.hh - 1;
        }
        const auto light = f.run(x, y);
        for (int c = 0; c < 3; ++c)
            need(std::abs(light[c] - expected) < .012f, name + ": wrong history blend");
        if (name == "background")
            need(light[3] == 0, "stale background history depth");
        if (name == "no_support")
            need(light[3] < 0, "unsupported receiver reused history");
        std::cout << "GI surface history " << name << ": light " << light[0] << '\n';
    }
}

#ifdef KIN_TEST_GI_HISTORY_WRAP
struct ObservedPass {
    SDL_GPUCommandBuffer* commands = nullptr;
    SDL_GPUTexture* coverage = nullptr;
};
struct Observer {
    SDL_GPUComputePipeline* resolve = nullptr;
    SDL_GPUTexture* coverage = nullptr;
    std::unordered_map<SDL_GPUComputePass*, ObservedPass> passes;
    std::unordered_map<SDL_GPUCommandBuffer*, bool> active;
    std::vector<bool> keeps;
    bool fail_resolve = false, fail_main = false, resolved_this_frame = false;
} observer;
u64 hash_code(const void* code, size_t size) {
    u64 value = 14695981039346656037ull;
    for (size_t i = 0; i < size; ++i)
        value = (value ^ static_cast<const u8*>(code)[i]) * 1099511628211ull;
    return value;
}

void history_contracts(Window& window) {
    auto backend = make_render_backend(window, false, true);
    need(backend && backend->name() == "SDL_GPU", "SDL_GPU renderer required");
    Renderer2D renderer{std::move(backend)};
    Renderer3D r3d{renderer};
    need(r3d.capabilities().global_illumination && r3d.capabilities().temporal_aa, "GI and TAA required");
    gpu_ready = true;
    auto view = r3d.create_view({96, 72}, {.samples = 1}).value_or({});
    auto other = r3d.create_view({96, 72}, {.samples = 1}).value_or({});
    auto floor = r3d.create_mesh(meshes::box({4, .1f, 4})).value_or({});
    need(view && other && floor, "history fixture unavailable");
    Scene3D scene;
    scene.camera = Camera3D::look_at({0, 2, 4}, {});
    scene.taa.enabled = true;
    scene.lights.sun.color = {};
    scene.lights.ambient = {.2f, .2f, .2f};
    scene.global_illumination = {.enabled = true, .area = {{-3, -1, -3}, {3, 3, 3}}, .cell = .2f};
    scene.global_illumination.probes.per_render = 256;
    need(scene.draw(floor, {}, {.color = colors::white}).ok(), "floor draw failed");
    unsigned frame = 0;
    const auto draw = [&](View3D& target, int expected_history, bool success = true) {
        const auto count = observer.keeps.size();
        observer.resolved_this_frame = false;
        renderer.clear(colors::black);
        const bool rendered = r3d.render(target, scene).ok();
        need(rendered == success, "unexpected render result");
        need(observer.keeps.size() == count + size_t(expected_history >= 0),
             "frame " + std::to_string(frame) + ": expected history " + std::to_string(expected_history) +
                 ", observed resolves " + std::to_string(observer.keeps.size() - count) + ", resolved " +
                 std::to_string(r3d.debug_stats().gi_resolved));
        if (expected_history >= 0)
            need(observer.keeps.back() == bool(expected_history), "GI history eligibility mismatch");
        renderer.present();
        ++frame;
    };
    draw(view, 0);
    draw(view, 1);
    view.reset_history();
    draw(view, 0);
    draw(view, 1);
    scene.camera.position.x += .5f;
    view.reset_history();
    draw(view, 0);
    draw(view, 1);
    scene.taa.enabled = false;
    draw(view, 0);
    scene.taa.enabled = true;
    draw(view, 0);
    draw(view, 1);
    scene.global_illumination.enabled = false;
    draw(view, -1);
    scene.global_illumination.enabled = true;
    draw(view, 0);
    draw(view, 1);
    scene.global_illumination.probes.enabled = false;
    draw(view, -1);
    scene.global_illumination.probes.enabled = true;
    draw(view, 0);
    draw(view, 1);
    need(r3d.resize(view, {95, 73}).ok(), "resize failed");
    draw(view, 0);
    draw(view, 1);
    draw(other, 0);
    draw(view, 1);
    draw(other, 1);
    draw(view, 1); // select this view's coverage target before injecting its resolve failure
    // A resolve pass may fail while the rest of the frame still advances TAA.
    observer.fail_resolve = true;
    draw(view, -1);
    need(!observer.fail_resolve, "resolve failure was not exercised");
    draw(view, 0);
    draw(view, 1);
    // A later main-pass failure must discard GI history even though resolve already recorded successfully.
    observer.fail_main = true;
    draw(view, 1, false);
    need(!observer.fail_main, "main-pass failure was not exercised");
    draw(view, 0);
    draw(view, 1);
    std::cout << "GI history: reset, cut, toggles, resize, two views, failed resolve and failed frame pass\n";
}
#endif
} // namespace

#ifdef KIN_TEST_GI_HISTORY_WRAP
extern "C" {
SDL_GPUComputePipeline* SDLCALL __real_SDL_CreateGPUComputePipeline(SDL_GPUDevice*,
                                                                    const SDL_GPUComputePipelineCreateInfo*);
SDL_GPUComputePass* SDLCALL __real_SDL_BeginGPUComputePass(SDL_GPUCommandBuffer*,
                                                           const SDL_GPUStorageTextureReadWriteBinding*, Uint32,
                                                           const SDL_GPUStorageBufferReadWriteBinding*, Uint32);
void SDLCALL __real_SDL_BindGPUComputePipeline(SDL_GPUComputePass*, SDL_GPUComputePipeline*);
void SDLCALL __real_SDL_PushGPUComputeUniformData(SDL_GPUCommandBuffer*, Uint32, const void*, Uint32);
void SDLCALL __real_SDL_EndGPUComputePass(SDL_GPUComputePass*);
SDL_GPURenderPass* SDLCALL __real_SDL_BeginGPURenderPass(SDL_GPUCommandBuffer*, const SDL_GPUColorTargetInfo*, Uint32,
                                                         const SDL_GPUDepthStencilTargetInfo*);

SDL_GPUComputePipeline* SDLCALL __wrap_SDL_CreateGPUComputePipeline(SDL_GPUDevice* device,
                                                                    const SDL_GPUComputePipelineCreateInfo* info) {
    auto* pipeline = __real_SDL_CreateGPUComputePipeline(device, info);
    const auto code = gi_test_shaders("gi_resolve.comp.spv");
    if (!code.empty() && hash_code(info->code, info->code_size) == hash_code(code.data(), code.size()))
        observer.resolve = pipeline;
    return pipeline;
}
SDL_GPUComputePass* SDLCALL __wrap_SDL_BeginGPUComputePass(SDL_GPUCommandBuffer* commands,
                                                           const SDL_GPUStorageTextureReadWriteBinding* textures,
                                                           Uint32 count,
                                                           const SDL_GPUStorageBufferReadWriteBinding* buffers,
                                                           Uint32 buffer_count) {
    if (observer.fail_resolve && count == 3 && textures[2].texture == observer.coverage) {
        observer.fail_resolve = false;
        return nullptr;
    }
    auto* pass = __real_SDL_BeginGPUComputePass(commands, textures, count, buffers, buffer_count);
    if (pass)
        observer.passes[pass] = {commands, count == 3 ? textures[2].texture : nullptr};
    return pass;
}
void SDLCALL __wrap_SDL_BindGPUComputePipeline(SDL_GPUComputePass* pass, SDL_GPUComputePipeline* pipeline) {
    const auto p = observer.passes.at(pass);
    const bool selected = observer.resolve && pipeline == observer.resolve;
    observer.active[p.commands] = selected;
    if (selected)
        observer.coverage = p.coverage;
    __real_SDL_BindGPUComputePipeline(pass, pipeline);
}
void SDLCALL __wrap_SDL_PushGPUComputeUniformData(SDL_GPUCommandBuffer* commands, Uint32 slot, const void* data,
                                                  Uint32 size) {
    if (observer.active[commands]) {
        need(slot == 0 && size == sizeof(Params), "resolve uniform layout drift");
        Params p;
        std::memcpy(&p, data, sizeof(p));
        need(p.previous_eye[3] == 0 || p.previous_eye[3] == 1, "invalid GI history bit");
        observer.keeps.push_back(p.previous_eye[3] != 0);
        observer.resolved_this_frame = true;
    }
    __real_SDL_PushGPUComputeUniformData(commands, slot, data, size);
}
void SDLCALL __wrap_SDL_EndGPUComputePass(SDL_GPUComputePass* pass) {
    observer.active[observer.passes.at(pass).commands] = false;
    observer.passes.erase(pass);
    __real_SDL_EndGPUComputePass(pass);
}
SDL_GPURenderPass* SDLCALL __wrap_SDL_BeginGPURenderPass(SDL_GPUCommandBuffer* commands,
                                                         const SDL_GPUColorTargetInfo* colours, Uint32 count,
                                                         const SDL_GPUDepthStencilTargetInfo* depth) {
    if (observer.fail_main && observer.resolved_this_frame) {
        observer.fail_main = false;
        return nullptr;
    }
    return __real_SDL_BeginGPURenderPass(commands, colours, count, depth);
}
}
#endif

int main(int argc, char** argv) {
    try {
        App app;
        auto& window = app.create_window({.title = "GI regression checks", .width = 96, .height = 72, .hidden = true});
        if (argc == 1)
            resolve_contracts(window);
        else if (std::string(argv[1]) == "placement")
            placement_contracts(window);
        else if (std::string(argv[1]) == "surface-hits")
            surface_hit_contracts(window);
        else if (std::string(argv[1]) == "surface-reconstruction")
            surface_reconstruction_contracts(window);
        else if (std::string(argv[1]) == "surface-history")
            surface_history_contracts(window);
#ifdef KIN_TEST_GI_HISTORY_WRAP
        else if (std::string(argv[1]) == "history")
            history_contracts(window);
#endif
        else
            return 2;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        const char* required = std::getenv("KIN_REQUIRE_GPU_TESTS");
        if (!gpu_ready && !(required && *required && std::strcmp(required, "0") != 0))
            return 77;
        return 1;
    }
}
