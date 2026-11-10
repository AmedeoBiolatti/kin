// write_scene_snapshot (kin/render3d/snapshot3d.hpp): a rendered scene written out for tools/cycles.

#include "hdr_encode3d.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <kin/assets/image.hpp>
#include <kin/render3d/reflection_probes3d.hpp>
#include <kin/render3d/snapshot3d.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace kin {
namespace {

// What a copy becomes in the truth.
enum class Kind : u8 { Plain, Water, Holdout };

struct Copy {
    const Scene3D::Draw* draw = nullptr;
    MeshInstance3D instance{};
    Material3D material{}; // with a planar surface taken off
    Kind kind = Kind::Plain;
    const Mat4f* joints = nullptr; // a skinned copy's pose
    bool baked = false;
    std::string mesh_file; // meshes/NNNN.bin
};

// A triangle's place in the pages: two triangles a cell (the cell's two halves, apart by a gap along the diagonal).
struct Cell {
    u32 copy = 0;
    u32 triangle[2] = {0, ~0u};
    u32 side = 0;
    u32 page = 0, x = 0, y = 0;
};

bool needs_bake(const Material3D& m) {
    return m.texture || m.metallic_roughness || m.normal || m.occlusion || m.emissive_texture || m.surface.valid();
}

LinearRgb linear(Color c) {
    return to_linear_rgb(c);
}

std::string number(f64 v) {
    if (!std::isfinite(v))
        return v > 0 ? "1e30" : v < 0 ? "-1e30" : "0";
    char text[32];
    std::snprintf(text, sizeof text, "%.7g", v);
    return text;
}
std::string vec(Vec3f v) {
    return "[" + number(v.x) + ", " + number(v.y) + ", " + number(v.z) + "]";
}
std::string rgb(LinearRgb c) {
    return "[" + number(c.r) + ", " + number(c.g) + ", " + number(c.b) + "]";
}
std::string json_string(const std::string& s) {
    std::string out = "\"";
    for (const char c : s) {
        if (c == '"' || c == '\\')
            out += '\\';
        out += c;
    }
    return out + "\"";
}

// A copy's triangles in its mesh's own space (posed, for a skinned copy): positions, normals, UVs and colours by
// corner, three a triangle.
struct Corners {
    std::vector<MeshVertex3D> vertices; // three a triangle
};

Corners corners(const MeshData3D& data, const Mat4f* joints) {
    Corners out;
    const std::size_t n = data.indices.empty() ? data.vertices.size() : data.indices.size();
    out.vertices.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        const u32 index = data.indices.empty() ? u32(i) : data.indices[i];
        MeshVertex3D v = data.vertices[index];
        if (joints && index < data.skin.size()) {
            const MeshSkinVertex3D& s = data.skin[index];
            Vec3f p{}, nrm{};
            for (int k = 0; k < 4; ++k) {
                if (s.weights[k] <= 0.0f)
                    continue;
                const Mat4f& m = joints[s.joints[k]];
                p = p + m.apply(v.position) * s.weights[k];
                nrm = nrm + m.apply_vector(v.normal) * s.weights[k];
            }
            v.position = p;
            const f32 l = length(nrm);
            v.normal = l > 1e-12f ? nrm / l : v.normal;
        }
        out.vertices.push_back(v);
    }
    return out;
}

void write_mesh(const std::filesystem::path& file, const std::vector<MeshVertex3D>& vertices,
                const std::vector<Vec2f>* uvs, const std::vector<u32>* pages) {
    // Unindexed (three vertices a triangle): position, normal, UV, linear colour, page (-1: none).
    std::vector<f32> out;
    out.reserve(vertices.size() * 13);
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        const MeshVertex3D& v = vertices[i];
        const Vec2f uv = uvs ? (*uvs)[i] : v.uv;
        const LinearColor c = to_linear(v.color);
        for (const f32 f : {v.position.x, v.position.y, v.position.z, v.normal.x, v.normal.y, v.normal.z, uv.x, uv.y,
                            c.r, c.g, c.b, c.a, pages ? f32((*pages)[i / 3]) : -1.0f})
            out.push_back(f);
    }
    std::ofstream(file, std::ios::binary)
        .write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size() * sizeof(f32)));
}

// Grows what was drawn (`covered`) into the texels round it, `steps` texels, each a mean of its drawn neighbours: a
// path tracer reading a texel at a triangle's edge, filtered, then reads the triangle's own.
void dilate(std::vector<f32>& texels, std::vector<u8>& covered, Vec2i size, int steps) {
    const int w = size.x, h = size.y;
    std::vector<u32> edge;
    for (int step = 0; step < steps; ++step) {
        edge.clear();
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                if (covered[std::size_t(y) * w + x])
                    continue;
                f32 sum[4] = {};
                int n = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int sx = x + dx, sy = y + dy;
                        if (sx < 0 || sy < 0 || sx >= w || sy >= h || !covered[std::size_t(sy) * w + sx])
                            continue;
                        const f32* t = &texels[(std::size_t(sy) * w + sx) * 4];
                        for (int c = 0; c < 4; ++c)
                            sum[c] += t[c];
                        ++n;
                    }
                if (n == 0)
                    continue;
                f32* t = &texels[(std::size_t(y) * w + x) * 4];
                for (int c = 0; c < 4; ++c)
                    t[c] = sum[c] / f32(n);
                edge.push_back(u32(y * w + x));
            }
        if (edge.empty())
            break;
        for (const u32 i : edge)
            covered[i] = 1;
    }
}

bool write_png(const std::filesystem::path& file, const std::vector<f32>& texels, Vec2i size, bool srgb, int channel,
               bool signed_unit) {
    Image image{size, std::vector<u8>(std::size_t(size.x) * size.y * 4)};
    for (std::size_t i = 0; i < std::size_t(size.x) * size.y; ++i)
        for (int c = 0; c < 4; ++c) {
            f32 v = c == 3 ? 1.0f : texels[i * 4 + std::size_t(channel >= 0 ? channel : c)];
            if (signed_unit && c < 3)
                v = v * 0.5f + 0.5f;
            if (srgb && c < 3)
                v = linear_to_srgb(std::clamp(v, 0.0f, 1.0f));
            image.rgba[i * 4 + c] = u8(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
        }
    return save_image(image, file);
}

bool write_hdr(const std::filesystem::path& file, const std::vector<f32>& texels, Vec2i size) {
    std::vector<f32> rgb(std::size_t(size.x) * size.y * 3);
    for (std::size_t i = 0; i < std::size_t(size.x) * size.y; ++i)
        for (int c = 0; c < 3; ++c)
            rgb[i * 3 + c] = texels[i * 4 + c];
    const std::string bytes = encode_hdr3d(size, rgb);
    std::ofstream out(file, std::ios::binary);
    return bool(out.write(bytes.data(), std::streamsize(bytes.size())));
}

} // namespace

Result3D<SnapshotStats3D> write_scene_snapshot(Renderer3D& renderer, const Scene3D& scene, Vec2i size,
                                               const std::filesystem::path& folder, const SnapshotOptions3D& options) {
    const auto fail = [](const std::string& why) {
        return Result3D<SnapshotStats3D>{std::nullopt, {"write_scene_snapshot: " + why}};
    };
    SnapshotStats3D stats;
    std::error_code error;
    std::filesystem::create_directories(folder / "meshes", error);
    std::filesystem::create_directories(folder / "pages", error);
    if (error)
        return fail("cannot make " + folder.string() + ": " + error.message());

    // Every copy drawn, its material as the truth takes it.
    std::vector<Copy> copies;
    for (const Scene3D::Draw& draw : scene.draws()) {
        const auto instances = draw.batch ? draw.batch.instances() : scene.instances();
        for (u32 i = 0; i < draw.count; ++i) {
            Copy c;
            c.draw = &draw;
            c.instance = instances[draw.first + i];
            c.material = draw.material;
            if (draw.skinned)
                c.joints = &scene.joint_matrices()[scene.skin_offsets()[draw.skin_first + i]];
            // (By the shader: a copy's traits may differ, water's from a mirror's.)
            const auto* surface = draw.material.surface.backend().get();
            if (options.window.valid() && surface == options.window.backend().get())
                c.kind = Kind::Holdout;
            else if (options.reflection.valid() && surface == options.reflection.backend().get()) {
                // The surface's own pictures (its reflection's, what is under water) are what the truth traces.
                if (draw.material.shader_params[36] > 0.0f) {
                    c.kind = Kind::Water;
                    c.material.texture = {};
                }
                c.material.surface = {};
                c.material.emissive_texture = {};
            }
            c.baked = c.kind != Kind::Holdout && needs_bake(c.material);
            copies.push_back(std::move(c));
        }
    }
    if (!scene.particle_draws().empty())
        stats.notes.push_back("particles are not written");
    if (scene.fog.enabled)
        stats.notes.push_back("fog is not written");
    if (scene.overlay_if_any())
        stats.notes.push_back("the overlay is not written");

    // Meshes, read back once each.
    std::unordered_map<const void*, MeshData3D> meshes;
    for (const Copy& c : copies) {
        const void* key = c.draw->mesh.backend().get();
        if (meshes.contains(key))
            continue;
        auto data = renderer.read_mesh(c.draw->mesh);
        if (!data)
            return fail(diagnostics3d_text(data.diagnostics));
        meshes.emplace(key, std::move(*data));
    }

    // Each copy's corners (posed); the baked ones' triangles into cells by their size in the world.
    std::vector<Corners> shapes(copies.size());
    std::vector<Cell> cells;
    for (u32 i = 0; i < copies.size(); ++i) {
        const Copy& c = copies[i];
        shapes[i] = corners(meshes.at(c.draw->mesh.backend().get()), c.joints);
        stats.triangles += shapes[i].vertices.size() / 3;
        if (!c.baked)
            continue;
        // Triangles by size class (a power of two), paired within a class.
        std::map<u32, std::vector<u32>> classes;
        const auto& v = shapes[i].vertices;
        for (u32 t = 0; t * 3 + 2 < v.size(); ++t) {
            const Vec3f a = c.instance.transform.apply(v[t * 3].position),
                        b = c.instance.transform.apply(v[t * 3 + 1].position),
                        d = c.instance.transform.apply(v[t * 3 + 2].position);
            const f32 area = 0.5f * length(cross(b - a, d - a));
            const f32 want = std::sqrt(2.0f * area) * options.texels_per_unit;
            u32 side = 8;
            while (side < options.max_cell && f32(side) < want)
                side *= 2;
            classes[side].push_back(t);
        }
        for (const auto& [side, list] : classes)
            for (std::size_t k = 0; k < list.size(); k += 2)
                cells.push_back({i, {list[k], k + 1 < list.size() ? list[k + 1] : ~0u}, side});
    }
    // Shelves, largest first (powers of two: rows of one height fill exactly).
    std::stable_sort(cells.begin(), cells.end(), [](const Cell& a, const Cell& b) { return a.side > b.side; });
    const u32 page = std::max(options.page_size, options.max_cell);
    u32 pages = 0, x = 0, y = 0, row = 0;
    for (Cell& cell : cells) {
        if (pages == 0)
            pages = 1;
        if (x + cell.side > page) {
            x = 0;
            y += row;
            row = 0;
        }
        if (y + cell.side > page) {
            ++pages;
            x = y = row = 0;
        }
        cell.page = pages - 1;
        cell.x = x;
        cell.y = y;
        x += cell.side;
        row = std::max(row, cell.side);
    }
    stats.pages = pages;

    // Atlas UVs of each baked copy's corners (u right, v down over its page), and each triangle's page.
    std::vector<std::vector<Vec2f>> atlas(copies.size());
    std::vector<std::vector<u32>> page_of(copies.size());
    for (u32 i = 0; i < copies.size(); ++i)
        if (copies[i].baked) {
            atlas[i].assign(shapes[i].vertices.size(), Vec2f{});
            page_of[i].assign(shapes[i].vertices.size() / 3, 0);
        }
    for (const Cell& cell : cells) {
        const f32 s = f32(cell.side), g = 1.5f, d = 1.5f, px = f32(cell.x), py = f32(cell.y), p = f32(page);
        // (Wound to face the bake's camera: a double-sided material turns a back face's normal round.)
        const std::array<std::array<Vec2f, 3>, 2> halves{
            std::array<Vec2f, 3>{Vec2f{px + g, py + g}, Vec2f{px + g, py + s - g - d}, Vec2f{px + s - g - d, py + g}},
            std::array<Vec2f, 3>{Vec2f{px + s - g, py + s - g}, Vec2f{px + s - g, py + g + d},
                                 Vec2f{px + g + d, py + s - g}}};
        for (int h = 0; h < 2; ++h) {
            if (cell.triangle[h] == ~0u)
                continue;
            for (int k = 0; k < 3; ++k)
                atlas[cell.copy][cell.triangle[h] * 3 + u32(k)] = {halves[h][k].x / p, halves[h][k].y / p};
            page_of[cell.copy][cell.triangle[h]] = cell.page;
        }
    }

    // The bakes: per page, each channel, every baked copy's triangles on it drawn over their cells (its own UVs in its
    // colour).
    std::vector<std::vector<Mesh3D>> bake_meshes(pages);
    std::vector<std::vector<Material3D>> bake_materials(pages);
    std::vector<std::vector<u32>> owners(pages); // the copy each of a page's bake meshes is of
    for (u32 i = 0; i < copies.size(); ++i) {
        if (!copies[i].baked)
            continue;
        const auto& v = shapes[i].vertices;
        Vec2f lo{1e30f, 1e30f}, hi{-1e30f, -1e30f};
        for (const MeshVertex3D& vertex : v) {
            lo = {std::min(lo.x, vertex.uv.x), std::min(lo.y, vertex.uv.y)};
            hi = {std::max(hi.x, vertex.uv.x), std::max(hi.y, vertex.uv.y)};
        }
        const Vec2f span{std::max(hi.x - lo.x, 1e-6f), std::max(hi.y - lo.y, 1e-6f)};
        std::map<u32, MeshData3D> on_page;
        for (std::size_t k = 0; k < v.size(); ++k) {
            MeshVertex3D out = v[k];
            const u32 u16x = u32(std::lround(std::clamp((v[k].uv.x - lo.x) / span.x, 0.0f, 1.0f) * 65535.0f));
            const u32 u16y = u32(std::lround(std::clamp((v[k].uv.y - lo.y) / span.y, 0.0f, 1.0f) * 65535.0f));
            out.color = Color{u8(u16x & 0xFF), u8(u16x >> 8), u8(u16y & 0xFF), u8(u16y >> 8)};
            out.uv = atlas[i][k];
            on_page[page_of[i][k / 3]].vertices.push_back(out);
        }
        Material3D m = copies[i].material;
        // The encoded UVs (0 to 1 over the copy's) back to its own, then the material's tiling.
        m.uv_offset = {m.uv_offset.x + m.uv_scale.x * lo.x, m.uv_offset.y + m.uv_scale.y * lo.y};
        m.uv_scale = {m.uv_scale.x * span.x, m.uv_scale.y * span.y};
        m.blend = Blend3D::Opaque;
        m.double_sided = true;
        m.casts_shadow = false;
        m.receives_shadow = false;
        m.wireframe = false;
        for (const auto& [p, data] : on_page) {
            auto mesh = renderer.create_mesh(data);
            if (!mesh)
                return fail(diagnostics3d_text(mesh.diagnostics));
            bake_meshes[p].push_back(std::move(*mesh));
            bake_materials[p].push_back(m);
            owners[p].push_back(i);
        }
        ++stats.baked;
    }
    const Vec2i page_size{i32(page), i32(page)};
    std::vector<u32> emitting; // the pages with an emission file
    Renderer2D& r2d = renderer.renderer();
    if (pages > 0) {
        Texture colour = r2d.create_render_attachment(page_size, TextureFormat::Rgba16Float);
        Texture depth = r2d.create_render_attachment(page_size, TextureFormat::Depth32Float);
        if (!colour || !depth)
            return fail("no render targets of " + std::to_string(page) + " texels");
        constexpr std::array channels{SurfaceChannel3D::BaseColor, SurfaceChannel3D::Alpha, SurfaceChannel3D::Normal,
                                      SurfaceChannel3D::OcclusionRoughnessMetallic, SurfaceChannel3D::Emission};
        constexpr std::array names{"base", "alpha", "normal", "orm", "emission"};
        for (u32 p = 0; p < pages; ++p) {
            std::vector<u8> covered;
            for (std::size_t ch = 0; ch < channels.size(); ++ch) {
                Scene3D bake;
                bake.camera = scene.camera;
                bake.camera.clip_plane = {0, 0, 0, 1};
                bake.camera.window = {0, 0, 1, 1};
                bake.camera.hidden_id = 0;
                bake.lights.sun.color = bake.lights.fill.color = bake.lights.ambient = {0, 0, 0};
                bake.time = scene.time;
                bake.cull = false;
                bake.anisotropy = 16;
                bake.debug.unwrap = true;
                bake.debug.surface_channel = channels[ch];
                for (std::size_t n = 0; n < bake_meshes[p].size(); ++n) {
                    const Copy& c = copies[owners[p][n]];
                    if (!bake.draw(bake_meshes[p][n], c.instance.transform, bake_materials[p][n], c.instance.tint))
                        return fail("a bake's draw was refused");
                }
                auto prepared = renderer.prepare(bake);
                if (!prepared)
                    return fail(diagnostics3d_text(prepared.diagnostics));
                if (!renderer.render_surface({.depth = depth, .linear_colour = colour}, *prepared))
                    return fail("a bake did not render: " + diagnostics3d_text(renderer.diagnostics()));
                auto texels = renderer.read_linear_texture(colour);
                if (!texels)
                    return fail(diagnostics3d_text(texels.diagnostics));
                std::vector<f32>& t = *texels;
                if (covered.empty()) {
                    covered.resize(std::size_t(page) * page);
                    for (std::size_t k = 0; k < covered.size(); ++k)
                        covered[k] = t[k * 4 + 3] > 0.5f ? 1 : 0;
                }
                if (channels[ch] == SurfaceChannel3D::Emission &&
                    std::none_of(t.begin(), t.end(), [&](const f32& f) { return (&f - t.data()) % 4 != 3 && f > 0; }))
                    continue; // (none: no file, and no emission in the truth)
                if (channels[ch] == SurfaceChannel3D::Emission)
                    emitting.push_back(p);
                std::vector<u8> grown = covered;
                dilate(t, grown, page_size, 6);
                const auto file = folder / "pages" / ("page" + std::to_string(p) + "_" + names[ch]);
                const bool ok = channels[ch] == SurfaceChannel3D::Emission
                                    ? write_hdr(file.string() + ".hdr", t, page_size)
                                : channels[ch] == SurfaceChannel3D::BaseColor
                                    ? write_png(file.string() + ".png", t, page_size, true, -1, false)
                                : channels[ch] == SurfaceChannel3D::Alpha
                                    ? write_png(file.string() + ".png", t, page_size, false, 0, false)
                                : channels[ch] == SurfaceChannel3D::Normal
                                    ? write_png(file.string() + ".png", t, page_size, false, -1, true)
                                    : write_png(file.string() + ".png", t, page_size, false, -1, false);
                if (!ok)
                    return fail("cannot write " + file.string());
            }
        }
    }

    // The sky as a panorama: an empty scene with the same sky and sun, its disc left out (the sun is a lamp).
    std::string sky_file;
    if (scene.sky.enabled) {
        Scene3D sky;
        sky.camera = scene.camera;
        sky.camera.clip_plane = {0, 0, 0, 1};
        sky.camera.window = {0, 0, 1, 1};
        sky.lights = scene.lights;
        sky.lights.points.clear();
        sky.lights.spots.clear();
        sky.lights.reflection_probes.clear();
        sky.sky = scene.sky;
        sky.sky.sun_disc = 0.0f;
        sky.time = scene.time;
        auto capture = renderer.create_reflection_capture(std::clamp(options.environment_side, 16u, 1024u));
        if (!capture)
            return fail(diagnostics3d_text(capture.diagnostics));
        if (!renderer.capture_reflection(*capture, sky, scene.camera.position) ||
            !renderer.save_reflection_capture(*capture, folder / "sky.hdr"))
            return fail("the sky's panorama: " + diagnostics3d_text(renderer.diagnostics()));
        sky_file = "sky.hdr";
    } else if (scene.lights.environment)
        stats.notes.push_back("the environment picture is not written (the ambient colour stands in)");

    // Meshes: a copy that is not baked shares its mesh's file with the others of it.
    std::unordered_map<const void*, std::string> shared;
    for (u32 i = 0; i < copies.size(); ++i) {
        Copy& c = copies[i];
        const void* key = c.draw->mesh.backend().get();
        if (!c.baked && !c.joints) {
            if (const auto it = shared.find(key); it != shared.end()) {
                c.mesh_file = it->second;
                continue;
            }
        }
        char name[32];
        std::snprintf(name, sizeof name, "meshes/%05u.bin", stats.meshes++);
        c.mesh_file = name;
        write_mesh(folder / name, shapes[i].vertices, c.baked ? &atlas[i] : nullptr, c.baked ? &page_of[i] : nullptr);
        if (!c.baked && !c.joints)
            shared.emplace(key, c.mesh_file);
    }

    // scene.json
    std::ostringstream j;
    const Camera3D& cam = scene.camera;
    const Lights3D& l = scene.lights;
    j << "{\n  \"schema\": \"kin.snapshot/1\",\n  \"size\": [" << size.x << ", " << size.y << "],\n";
    j << "  \"camera\": {\"position\": " << vec(cam.position)
      << ", \"forward\": " << vec(cam.rotation.rotate({0, 0, -1}))
      << ", \"up\": " << vec(cam.rotation.rotate({0, 1, 0}))
      << ", \"orthographic\": " << (cam.projection == Projection3D::Orthographic ? "true" : "false")
      << ", \"fov\": " << number(cam.fov) << ", \"height\": " << number(cam.height)
      << ", \"near\": " << number(cam.z_near) << ", \"far\": " << number(std::isfinite(cam.z_far) ? cam.z_far : 1e5f)
      << "},\n";
    const ViewOptions3D& v = options.view;
    const LinearColor bg = to_linear(v.background);
    j << "  \"view\": {\"hdr\": " << (v.hdr ? "true" : "false") << ", \"exposure\": " << number(v.exposure)
      << ", \"bloom\": " << number(v.bloom) << ", \"background\": [" << number(bg.r) << ", " << number(bg.g) << ", "
      << number(bg.b) << ", " << number(bg.a) << "]},\n";
    j << "  \"sun\": {\"direction\": " << vec(l.sun.direction) << ", \"color\": " << rgb(l.sun.color)
      << ", \"angle\": " << number(scene.sky.enabled ? scene.sky.sun_size : 0.53f)
      << ", \"shadow\": " << (l.shadow_area.size().x > 0 && l.sun_shadow_strength > 0 ? "true" : "false") << "},\n";
    j << "  \"fill\": {\"direction\": " << vec(l.fill.direction) << ", \"color\": " << rgb(l.fill.color)
      << ", \"shadow\": " << (l.fill_shadow.enabled ? "true" : "false") << "},\n";
    j << "  \"ambient\": " << rgb(l.ambient)
      << ",\n  \"ground_ambient\": " << (l.ground_ambient.enabled ? rgb(l.ground_ambient.color) : std::string("null"))
      << ",\n";
    j << "  \"sky\": " << (sky_file.empty() ? std::string("null") : json_string(sky_file))
      << ",\n  \"sky_lights_scene\": " << (scene.sky.enabled && l.sky_lights_scene ? "true" : "false")
      << ",\n  \"environment_intensity\": " << number(l.environment_intensity)
      << ",\n  \"specular_multiscatter\": " << (scene.specular_multiscatter ? "true" : "false") << ",\n";
    j << "  \"points\": [";
    for (std::size_t i = 0; i < l.points.size(); ++i) {
        const PointLightSource3D& p = l.points[i];
        j << (i ? ",\n    " : "\n    ") << "{\"position\": " << vec(p.position) << ", \"color\": " << rgb(p.color)
          << ", \"radius\": " << number(p.radius) << ", \"size\": " << number(p.size)
          << ", \"shadow\": " << (p.shadow_enabled ? "true" : "false") << "}";
    }
    j << (l.points.empty() ? "" : "\n  ") << "],\n  \"spots\": [";
    for (std::size_t i = 0; i < l.spots.size(); ++i) {
        const SpotLightSource3D& p = l.spots[i];
        j << (i ? ",\n    " : "\n    ") << "{\"position\": " << vec(p.position)
          << ", \"direction\": " << vec(p.direction) << ", \"color\": " << rgb(p.color)
          << ", \"radius\": " << number(p.radius) << ", \"size\": " << number(p.size)
          << ", \"inner\": " << number(p.inner_angle) << ", \"outer\": " << number(p.outer_angle)
          << ", \"shadow\": " << (p.shadow_enabled ? "true" : "false") << "}";
    }
    j << (l.spots.empty() ? "" : "\n  ") << "],\n  \"fog\": {\"enabled\": " << (scene.fog.enabled ? "true" : "false")
      << ", \"density\": " << number(scene.fog.density) << "},\n";
    j << "  \"page_size\": " << page << ",\n  \"pages\": " << pages << ",\n  \"emission_pages\": [";
    for (std::size_t i = 0; i < emitting.size(); ++i)
        j << (i ? ", " : "") << emitting[i];
    j << "],\n  \"objects\": [";
    for (u32 i = 0; i < copies.size(); ++i) {
        const Copy& c = copies[i];
        const Material3D& m = c.material;
        const LinearColor tint = to_linear(c.instance.tint), base = to_linear(m.color);
        const LinearRgb colour{base.r * tint.r, base.g * tint.g, base.b * tint.b};
        // The emission without maps: the base colour's share, or a colour of its own (the vertex colours times the
        // first, as kin's are).
        const LinearRgb emission =
            m.emission_mode == EmissionMode3D::BaseRelative ? colour * m.emissive : m.emission_color * m.emissive;
        j << (i ? ",\n    " : "\n    ") << "{\"mesh\": " << json_string(c.mesh_file) << ", \"transform\": [";
        for (int k = 0; k < 16; ++k)
            j << (k ? ", " : "") << number(c.instance.transform.m[k]);
        j << "], \"kind\": \""
          << (c.kind == Kind::Holdout ? "holdout"
              : c.kind == Kind::Water ? "water"
                                      : "plain")
          << "\"";
        j << ", \"baked\": " << (c.baked ? "true" : "false");
        j << ", \"color\": " << rgb(colour) << ", \"alpha\": " << number(base.a * tint.a)
          << ", \"metallic\": " << number(m.metallic) << ", \"roughness\": " << number(m.roughness)
          << ", \"emission\": " << rgb(emission)
          << ", \"emission_from_base\": " << (m.emission_mode == EmissionMode3D::BaseRelative ? "true" : "false")
          << ", \"unlit\": " << (m.unlit ? "true" : "false")
          << ", \"double_sided\": " << (m.double_sided ? "true" : "false") << ", \"blend\": \""
          << (m.blend == Blend3D::Opaque ? "opaque"
              : m.blend == Blend3D::Mask ? "mask"
                                         : "blend")
          << "\", \"alpha_cutoff\": " << number(m.alpha_cutoff)
          << ", \"casts_shadow\": " << (m.casts_shadow ? "true" : "false");
        if (c.kind == Kind::Water)
            j << ", \"transmission\": " << number(std::clamp(c.draw->material.shader_params[36], 0.0f, 1.0f));
        j << "}";
        ++stats.objects;
    }
    j << (copies.empty() ? "" : "\n  ") << "],\n  \"notes\": [";
    for (std::size_t i = 0; i < stats.notes.size(); ++i)
        j << (i ? ", " : "") << json_string(stats.notes[i]);
    j << "]\n}\n";
    std::ofstream out(folder / "scene.json");
    if (!(out << j.str()))
        return fail("cannot write " + (folder / "scene.json").string());
    return result3d(std::move(stats));
}

} // namespace kin
