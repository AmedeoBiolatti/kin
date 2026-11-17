#include "embedded_gpu_shader.hpp"
#include "gpu_device.hpp"
#include "gpu_renderer2d_backend.hpp"
#include "gpu_renderer3d.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace kin::gpu {
namespace {
// Keep in step with particle_simulate.comp.glsl (only vec4/mat4/uvec4).
struct Simulation {
    f32 transform[16];
    u32 counts[4]; // capacity, first slot, births, birth sequence
    u32 flags[4];  // seed, clear, shape, surface
    f32 time[4];   // dt, drag, wind influence, spread radians
    f32 extent[4], velocity_min[4], velocity_max[4], gravity[4], wind[4];
    f32 lifetime_speed[4], size_rotation[4], spin[4], sizes[4];
    f32 colour_start[4], colour_end[4], curves[4], ground[4], atlas[4];
};
static_assert(sizeof(Simulation) == 336);
void put(f32* p, Vec3f v, f32 w = 0) {
    p[0] = v.x;
    p[1] = v.y;
    p[2] = v.z;
    p[3] = w;
}
} // namespace
std::shared_ptr<IGpuParticleBackend3D> GpuRenderer3D::create_gpu_particles(const ParticleDesc3D& desc, u32 seed) {
    if (!_capabilities.gpu_particles)
        return {};
    if (desc.look.texture) {
        const auto* t = dynamic_cast<const GpuTextureBackend*>(desc.look.texture.backend().get());
        if (!t || !t->texture().belongs_to(_device->handle()) || desc.look.texture.array_layers() ||
            !(t->texture().usage() & SDL_GPU_TEXTUREUSAGE_SAMPLER) ||
            (desc.look.texture.format() != TextureFormat::Rgba8 &&
             !texture_format_compressed(desc.look.texture.format())))
            return {};
    }
    auto p = std::make_shared<GpuParticles3D>();
    p->desc = desc;
    p->seed = seed;
    p->serial = ++_particle_serial;
    const auto usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE |
                       SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
    p->state = std::make_shared<GpuBuffer>(_device->create_buffer(usage, nullptr, desc.max_particles * 32));
    p->sprites = std::make_shared<GpuBuffer>(_device->create_buffer(usage, nullptr, desc.max_particles * 64));
    if (!*p->state || !*p->sprites || !update_gpu_particles(p, {.clear = true}))
        return {};
    return p;
}
bool GpuRenderer3D::update_gpu_particles(const std::shared_ptr<IGpuParticleBackend3D>& source,
                                         const GpuParticleUpdate3D& u) {
    auto p = std::dynamic_pointer_cast<GpuParticles3D>(source);
    if (!p || !p->sprites->belongs_to(_device->handle()) || !_particle_simulate)
        return false;
    const auto& d = p->desc;
    const bool clear = u.clear || !p->initialized;
    const f64 carry = (clear ? 0 : p->carry) + (u.emitting ? f64(d.rate) * u.dt : 0);
    const u64 births = u64(std::floor(carry)) + u.burst;
    const u32 count = u32(std::min<u64>(births, d.max_particles));
    // Keep the latest capacity births if more than one ring is requested.
    const u32 first = u32(((clear ? 0 : p->next) + births - count) % d.max_particles);
    Simulation s{};
    std::memcpy(s.transform, u.transform.m, sizeof(s.transform));
    s.counts[0] = d.max_particles;
    s.counts[1] = first;
    s.counts[2] = count;
    const u64 first_birth = (clear ? 0 : p->birth) + births - count;
    s.counts[3] = u32(first_birth);
    s.flags[0] = p->seed;
    s.flags[1] = clear;
    s.flags[2] = static_cast<u32>(d.shape);
    s.flags[3] = d.surface_only;
    constexpr f32 radians = 3.14159265358979323846f / 180;
    s.time[0] = f32(u.dt);
    s.time[1] = d.drag;
    s.time[2] = d.wind_influence;
    s.time[3] = std::clamp(d.shape == ParticleShape3D::Cone ? d.cone_angle : d.spread, 0.f, 180.f) * radians;
    put(s.extent, d.extent);
    put(s.velocity_min, d.velocity_min);
    put(s.velocity_max, d.velocity_max);
    put(s.gravity, d.gravity);
    put(s.wind, u.wind);
    s.lifetime_speed[0] = d.lifetime.min;
    s.lifetime_speed[1] = d.lifetime.max;
    s.lifetime_speed[2] = d.speed.min;
    s.lifetime_speed[3] = d.speed.max;
    s.size_rotation[0] = d.size.min;
    s.size_rotation[1] = d.size.max;
    s.size_rotation[2] = d.rotation.min * radians;
    s.size_rotation[3] = d.rotation.max * radians;
    s.spin[0] = d.spin.min * radians;
    s.spin[1] = d.spin.max * radians;
    s.spin[2] = std::bit_cast<f32>(u32(first_birth >> 32));
    s.spin[3] = std::bit_cast<f32>(p->epoch + u32(clear));
    s.sizes[0] = d.size_start;
    s.sizes[1] = d.size_mid;
    s.sizes[2] = d.size_end;
    put(s.colour_start, {d.colour_start.r, d.colour_start.g, d.colour_start.b}, d.alpha_start);
    put(s.colour_end, {d.colour_end.r, d.colour_end.g, d.colour_end.b}, d.alpha_end);
    s.curves[0] = d.fade_in;
    s.curves[1] = d.colour_variation;
    s.curves[2] = d.frames_per_life;
    s.curves[3] = d.frame_random ? 1.f : 0.f;
    s.ground[0] = d.ground ? 1.f : 0.f;
    s.ground[1] = d.ground_height;
    s.ground[2] = d.bounce;
    const u64 frames = u64(std::max(d.look.atlas_columns, 1u)) * std::max(d.look.atlas_rows, 1u);
    s.atlas[0] = f32(d.look.atlas_frames ? std::min<u64>(d.look.atlas_frames, frames) : frames);
    const u64 regular = u64(std::floor(carry)), skipped = births - count;
    s.atlas[1] = f32(regular > skipped ? regular - skipped : 0);
    s.atlas[2] =
        d.rate > 0 && u.emitting ? f32(std::max(0.0, u.dt - (f64(skipped) + 1 - (clear ? 0 : p->carry)) / d.rate)) : 0;
    s.atlas[3] = d.rate > 0 ? 1.f / d.rate : 0;
    _owner->ensure_frame();
    _owner->flush_to_frame();
    auto* commands = _owner->_frame->command_buffer();
    SDL_GPUStorageBufferReadWriteBinding outputs[]{{p->state->handle(), false}, {p->sprites->handle(), false}};
    auto* pass = SDL_BeginGPUComputePass(commands, nullptr, 0, outputs, 2);
    if (!pass)
        return false;
    SDL_BindGPUComputePipeline(pass, _particle_simulate);
    SDL_PushGPUComputeUniformData(commands, 0, &s, sizeof(s));
    SDL_DispatchGPUCompute(pass, (d.max_particles + 63) / 64, 1, 1);
    SDL_EndGPUComputePass(pass);
    _owner->_retained_particles.push_back(p);
    p->initialized = true;
    if (clear || u.dt > 0 || births)
        ++p->revision;
    p->next = u32((u64(first) + count) % d.max_particles);
    p->birth = (clear ? 0 : p->birth) + births;
    p->epoch += u32(clear);
    p->carry = carry - std::floor(carry);
    p->origin = u.transform.apply({});
    return true;
}
void GpuRenderer3D::prepare_gpu_particle_order(const std::shared_ptr<GpuParticles3D>& p, const Camera3D& camera,
                                               const GpuFrustum& frustum, bool cull) {
    if (!p->desc.look.sort)
        return;
    const u32 length = std::bit_ceil(p->desc.max_particles);
    if (!p->order)
        p->order = std::make_shared<GpuDataBuffer>(_device->create_buffer(
            SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE |
                SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ,
            nullptr, length * 8));
    struct Initial {
        f32 eye[4], forward[4], planes[7][4], stretch[4];
        u32 counts[4];
    } u{};
    static_assert(sizeof(Initial) == 176);
    put(u.eye, camera.position);
    put(u.forward, camera.forward());
    std::memcpy(u.planes, frustum.planes.data(), sizeof(u.planes));
    u.stretch[0] = p->desc.look.stretch;
    u.stretch[1] = p->desc.look.stretch_min;
    u.stretch[2] = p->desc.look.facing == ParticleFacing3D::Stretched ? 1.f : 0.f;
    u.counts[0] = p->desc.max_particles;
    u.counts[1] = length;
    u.counts[2] = cull ? frustum.count : 0;
    _owner->ensure_frame();
    _owner->flush_to_frame();
    auto* commands = _owner->_frame->command_buffer();
    SDL_GPUStorageBufferReadWriteBinding output{p->order->buffer().handle(), false};
    auto* pass = SDL_BeginGPUComputePass(commands, nullptr, 0, &output, 1);
    if (!pass)
        throw std::runtime_error("GPU particle order pass could not begin");
    SDL_BindGPUComputePipeline(pass, _particle_order_init);
    auto* source = p->sprites->handle();
    SDL_BindGPUComputeStorageBuffers(pass, 0, &source, 1);
    SDL_PushGPUComputeUniformData(commands, 0, &u, sizeof(u));
    SDL_DispatchGPUCompute(pass, (length + 255) / 256, 1, 1);
    SDL_EndGPUComputePass(pass);
    for (u32 width = 512; width <= length; width *= 2) {
        const auto merge = [&](u32 stride) {
            auto* merge_pass = SDL_BeginGPUComputePass(commands, nullptr, 0, &output, 1);
            if (!merge_pass)
                throw std::runtime_error("GPU particle merge pass could not begin");
            SDL_BindGPUComputePipeline(merge_pass, _particle_order_merge);
            const u32 params[]{length, width, stride, 0};
            SDL_PushGPUComputeUniformData(commands, 0, params, sizeof(params));
            SDL_DispatchGPUCompute(merge_pass, (length + 255) / 256, 1, 1);
            SDL_EndGPUComputePass(merge_pass);
        };
        for (u32 stride = width / 2; stride >= 256; stride /= 2)
            merge(stride);
        merge(0);
    }
    _owner->_retained_buffers.push_back(p->order);
}
} // namespace kin::gpu
