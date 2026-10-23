#include <cassert>
// CPU costs of kin/render3d that need no window: `batch` (InstanceBatch3D edits: bounds and the change history a
// renderer reads), `wind` (a ParticleSystem3D of 20,000 live particles in gusts and turbulence). One line a case;
// argv[1] picks the mode, default all. Medians of many frames, so a quiet machine matters (docs/rendering_3d.md,
// "Costs").

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <kin/render3d/particles3d.hpp>
#include <kin/render3d/scene3d.hpp>
#include <memory>
#include <vector>

using namespace kin;

namespace {

struct Lcg {
    u32 s = 12345;
    u32 next() {
        s = s * 1664525u + 1013904223u;
        return s >> 8;
    }
    f32 unit() { return static_cast<f32>(next()) / static_cast<f32>(1u << 24); }
};

double now_ms() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

// A batch of a mesh of 1 x 2 x 0.5 m in 8 groups of copies spread over a field; edits move copies a little, as a game
// does.
struct Field {
    static constexpr u32 count = 100'000, group_count = 8;
    InstanceBatch3D batch;
    Lcg rng;
    Field() {
        struct Fake final : IMeshBackend3D {};
        const Mesh3D mesh{std::make_shared<Fake>(), 24, 36, {{-0.5f, -1.0f, -0.25f}, {0.5f, 1.0f, 0.25f}}};
        std::vector<MeshInstance3D> copies(count);
        std::vector<InstanceGroup3D> groups;
        const u32 per = count / group_count;
        for (u32 g = 0; g < group_count; ++g) {
            groups.push_back({g * per, per});
            for (u32 i = 0; i < per; ++i) {
                const f32 x = static_cast<f32>(g) * 100.0f + rng.unit() * 90.0f, z = rng.unit() * 400.0f;
                copies[g * per + i] = {Mat4f::translation({x, rng.unit() * 5.0f, z}), colors::white, g * per + i + 1};
            }
        }
        batch = InstanceBatch3D::create(mesh, std::move(copies), std::move(groups)).value_or({});
    }
};

// What a renderer does with a batch each frame: the layout, the groups' bounds, the changed range, and the changed
// ranges one by one (a batch that cannot say gives the whole of it, in one).
struct Reads {
    InstanceBatch3D::ChangedRange changed{};
    std::size_t ranges = 0;
    u32 covered = 0;
    double sink = 0.0;
};
void read_batch(const InstanceBatch3D& batch, u64 since, Reads& out) {
    static std::vector<InstanceBatch3D::ChangedRange> ranges;
    out.sink += static_cast<double>(batch.layout_revision());
    for (const InstanceGroup3D& g : batch.groups())
        out.sink += g.world_bounds.max.x - g.world_bounds.min.x;
    out.changed = batch.changed_since(since);
    ranges.clear();
    batch.changed_ranges_since(since, ranges);
    out.ranges = ranges.size();
    out.covered = 0;
    for (const auto& r : ranges)
        out.covered += r.count;
}

// `edits` edits of `span` copies a frame (at scattered places, or spread evenly with a little jitter when span > 1).
// The values are made beforehand, 8 frames' worth that the run cycles through, so that only the batch is timed.
void batch_case(const char* name, u32 edits, u32 span) {
    Field f;
    struct Edit {
        u32 first;
        std::size_t at;
    };
    constexpr int cycle = 8, frames = 100;
    std::vector<std::vector<Edit>> plan(cycle);
    std::vector<MeshInstance3D> values;
    for (auto& frame : plan)
        for (u32 e = 0; e < edits; ++e) {
            const u32 first = span == 1 ? f.rng.next() % Field::count
                                        : (e * (Field::count / edits) + f.rng.next() % 1000) % (Field::count - span);
            frame.push_back({first, values.size()});
            for (u32 k = 0; k < span; ++k) {
                MeshInstance3D m = f.batch.instances()[first + k];
                m.transform.m[12] += (f.rng.unit() - 0.5f) * 0.2f;
                m.transform.m[14] += (f.rng.unit() - 0.5f) * 0.2f;
                values.push_back(m);
            }
        }
    std::vector<double> total, update_ms, read_ms;
    Reads reads;
    for (int frame = 0; frame < frames + 5; ++frame) {
        const u64 since = f.batch.revision();
        const double t0 = now_ms();
        for (const Edit& e : plan[frame % cycle])
            assert(f.batch.update(e.first, std::span<const MeshInstance3D>(values).subspan(e.at, span)));
        const double t1 = now_ms();
        read_batch(f.batch, since, reads);
        const double t2 = now_ms();
        if (frame >= 5) {
            total.push_back(t2 - t0);
            update_ms.push_back(t1 - t0);
            read_ms.push_back(t2 - t1);
        }
    }
    std::printf("batch %-26s %7.3f ms/frame (updates %7.3f, reads %7.3f), changed_since: first %u count %u, %zu ranges "
                "cover %u of %u [%g]\n",
                name, median(total), median(update_ms), median(read_ms), reads.changed.first, reads.changed.count,
                reads.ranges, reads.covered, Field::count, reads.sink > 0.0 ? 0.0 : 1.0);
}

void batch_mode() {
    batch_case("2000 edits of 1 (scattered)", 2000, 1);
    batch_case("16 edits of 1000 (contiguous)", 16, 1000);
}

// Particles alive in a cloud, the air with gusts and turbulence (or none): the cost of one update(1/60) (two 1/120 s
// steps).
void wind_case(const char* name, Vec3f extent, bool windy = true) {
    ParticleSystem3D system{5};
    ParticleDesc3D& d = system.desc;
    d.shape = ParticleShape3D::Box;
    d.extent = extent;
    d.rate = 0.0f;
    d.max_particles = 20000;
    d.lifetime = {60.0f, 60.0f};
    d.speed = {0.0f, 0.5f};
    d.spread = 180.0f;
    d.gravity = {0.0f, -1.2f, 0.0f};
    d.drag = 1.5f;
    d.wind_influence = 0.85f;
    system.transform = Mat4f::translation({0.0f, 12.0f, 0.0f});
    system.burst(20000);
    const Wind3D wind{.direction = {1.0f, 0.0f, 0.25f},
                      .speed = 6.0f,
                      .gust_strength = 0.5f,
                      .gust_period = 8.0f,
                      .gust_length = 60.0f,
                      .turbulence = 0.8f,
                      .turbulence_length = 5.0f};
    std::vector<double> ms;
    for (int i = 0; i < 125; ++i) {
        const double t0 = now_ms();
        system.update(1.0 / 60.0, windy ? &wind : nullptr);
        if (i >= 5)
            ms.push_back(now_ms() - t0);
    }
    const Vec3f p = system.particles()[0].position;
    std::printf("wind  %-26s %7.3f ms/update(1/60) (median of %zu), %zu particles alive, hash %016llx [%g]\n", name,
                median(ms), ms.size(), system.count(), static_cast<unsigned long long>(system.state_hash()),
                p.x * 0.0f);
}

void wind_mode() {
    wind_case("spread (36x24x36 m box)", {18.0f, 12.0f, 18.0f});
    wind_case("compact (3x3x3 m box)", {1.5f, 1.5f, 1.5f});
    wind_case("no wind, drag only", {18.0f, 12.0f, 18.0f}, false);
}

} // namespace

int main(int argc, char** argv) {
    const char* mode = argc > 1 ? argv[1] : "all";
    const bool all = !std::strcmp(mode, "all");
    if (all || !std::strcmp(mode, "batch"))
        batch_mode();
    if (all || !std::strcmp(mode, "wind"))
        wind_mode();
    return 0;
}
