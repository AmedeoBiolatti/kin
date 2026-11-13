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
        SDL_GPUTransferBufferCreateInfo info{.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
                                             .size = u32(values.size() * 4)};
        auto* transfer = SDL_CreateGPUTransferBuffer(d, &info);
        need(transfer, "upload buffer unavailable");
        auto* mapped = SDL_MapGPUTransferBuffer(d, transfer, false);
        need(mapped, "upload map failed");
        std::memcpy(mapped, values.data(), values.size() * 4);
        SDL_UnmapGPUTransferBuffer(d, transfer);
        auto* cmd = SDL_AcquireGPUCommandBuffer(d);
        need(cmd, "upload command failed");
        auto* pass = SDL_BeginGPUCopyPass(cmd);
        need(pass, "upload pass failed");
        SDL_GPUTextureTransferInfo source{
            .transfer_buffer = transfer, .pixels_per_row = u32(w), .rows_per_layer = u32(h)};
        SDL_GPUTextureRegion target{.texture = inputs[input].handle(), .w = u32(w), .h = u32(h), .d = u32(layers)};
        SDL_UploadToGPUTexture(pass, &source, &target, false);
        SDL_EndGPUCopyPass(pass);
        complete(d, cmd);
        SDL_ReleaseGPUTransferBuffer(d, transfer);
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
}

void surface_reconstruction_contracts(Window& window) {
    gpu::GpuDevice device(window);
    gpu_ready = true;
    for (const std::string name : {"plane_hole", "wide_hole", "perspective", "dark_neighbours", "window_fade",
                                   "outside_window", "untraced", "normal_boundary", "depth_boundary", "disconnected",
                                   "world_bound", "legitimate_dark", "no_donor", "odd_edge"}) {
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
            need(std::abs(result[c] - expected) < .001f, name + ": borrowed incorrect surface lighting");
        }
        const float expected_coverage = repaired ? (name == "window_fade" ? .4f : 1.f) : shares[(cy * f.hw + cx) * 2];
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
