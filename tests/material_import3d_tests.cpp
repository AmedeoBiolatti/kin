// Other engines' materials as kin's (kin/render3d/material_import3d.hpp), on the CPU: Godot's StandardMaterial3D and
// ORMMaterial3D read into kin.materials3d/1 with the textures kin needs (metallic and roughness channels packed into
// one map, occlusion moved to R, an added emission colour put into its map); what kin can't show is noted; files that
// aren't such materials are refused. With KIN_TEST_GODOT_PROJECT (a Godot project folder), every material of it is
// imported (a dry run).
#include <kin/assets/image.hpp>
#include <kin/render3d/material_asset3d.hpp>
#include <kin/render3d/material_import3d.hpp>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {
using namespace kin;
namespace fs = std::filesystem;

#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            std::cerr << __FILE__ << ":" << __LINE__ << ": " #condition "\n";                                          \
            std::exit(1);                                                                                              \
        }                                                                                                              \
    } while (0)

struct Scratch {
    fs::path path = fs::temp_directory_path() /
                    ("kin-material-import-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Scratch() { fs::create_directories(path); }
    ~Scratch() {
        std::error_code error;
        fs::remove_all(path, error);
    }
};

void write(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << text;
}

// A picture of 4 x 2 texels, each (r, g, b, a) made from its index.
Image picture(u8 r, u8 g, u8 b, u8 a = 255) {
    Image image{{4, 2}, std::vector<u8>(4 * 2 * 4)};
    for (int i = 0; i < 8; ++i) {
        image.rgba[i * 4 + 0] = u8(r + i);
        image.rgba[i * 4 + 1] = u8(g + i);
        image.rgba[i * 4 + 2] = u8(b + i);
        image.rgba[i * 4 + 3] = a;
    }
    return image;
}

bool has_note(const ImportedMaterial3D& m, std::string_view what) {
    return std::any_of(m.notes.begin(), m.notes.end(), [&](const std::string& n) { return n.find(what) == 0; });
}

void standard_material() {
    Scratch scratch;
    const fs::path project = scratch.path / "game", content = scratch.path;
    write(project / "project.godot", "[rendering]\nlights_and_shadows/use_physical_light_units=true\n");
    CHECK(save_image(picture(10, 20, 30), project / "tex/albedo.png"));
    CHECK(save_image(picture(100, 0, 0), project / "tex/metal.png")); // metallic in R
    CHECK(save_image(picture(0, 50, 0), project / "tex/rough.png"));  // roughness in G
    CHECK(save_image(picture(0, 0, 200), project / "tex/ao.png"));    // occlusion in B
    CHECK(save_image(picture(40, 40, 40), project / "tex/glow.png"));
    write(project / "materials/brick.tres", R"([gd_resource type="StandardMaterial3D" load_steps=6 format=3 uid="uid://x"]

[ext_resource type="Texture2D" uid="uid://a" path="res://tex/albedo.png" id="1_a"]
[ext_resource type="Texture2D" uid="uid://m" path="res://tex/metal.png" id="2_m"]
[ext_resource type="Texture2D" uid="uid://r" path="res://tex/rough.png" id="3_r"]
[ext_resource type="Texture2D" uid="uid://o" path="res://tex/ao.png" id="4_o"]
[ext_resource type="Texture2D" uid="uid://e" path="res://tex/glow.png" id="5_e"]

[resource]
resource_name = "Brick"
transparency = 2
alpha_scissor_threshold = 0.3
cull_mode = 2
albedo_color = Color(1, 0.5, 0.25, 1)
albedo_texture = ExtResource("1_a")
metallic = 0.5
metallic_texture = ExtResource("2_m")
roughness = 0.8
roughness_texture = ExtResource("3_r")
roughness_texture_channel = 1
normal_enabled = false
ao_enabled = true
ao_texture = ExtResource("4_o")
ao_texture_channel = 2
emission_enabled = true
emission = Color(0.5, 0, 0, 1)
emission_energy_multiplier = 2.0
emission_intensity = 10.0
emission_texture = ExtResource("5_e")
backlight_enabled = true
texture_filter = 0
texture_repeat = false
)");
    const auto result = import_godot_material(
        project / "materials/brick.tres", {.project = project, .content = content, .out = content / "kin"});
    if (!result) {
        for (const auto& d : result.diagnostics)
            std::cerr << d << '\n';
        CHECK(false);
    }
    const auto& m = *result;
    const auto& p = m.asset.base.material;
    CHECK(m.name == "Brick");
    CHECK(p.color && p.color->r == 255 && p.color->g == 128 && p.color->b == 64 && p.color->a == 255);
    CHECK(p.texture && p.texture->asset == fs::path("game/tex/albedo.png")); // in place: inside the content
    CHECK(p.texture->sampler && p.texture->sampler->min == TextureFilter3D::Nearest &&
          p.texture->sampler->mip == MipFilter3D::None && p.texture->sampler->wrap_u == Wrap3D::Clamp);
    CHECK(*p.metallic == 0.5f && std::abs(*p.roughness - 0.8f) < 1e-6f);
    CHECK(*p.blend == Blend3D::Mask && std::abs(*p.alpha_cutoff - 0.3f) < 1e-6f && *p.double_sided);
    CHECK(p.normal && p.normal->asset.empty()); // off: the model's own cleared
    // Metallic and roughness packed as kin's map: roughness's G in G, metallic's R in B.
    const Image packed = load_image(content / p.metallic_roughness->asset);
    CHECK(packed.size == (Vec2i{4, 2}));
    for (int i = 0; i < 8; ++i)
        CHECK(packed.rgba[i * 4 + 1] == 50 + i && packed.rgba[i * 4 + 2] == 100 + i);
    // Occlusion's B moved to R.
    const Image ao = load_image(content / p.occlusion->asset);
    for (int i = 0; i < 8; ++i)
        CHECK(ao.rgba[i * 4] == 200 + i);
    // Emission: colour added to the map (linear), the brightest texel 1, its strength the energy (2 x 10 nits).
    CHECK(*p.emission_mode == EmissionMode3D::Independent && p.emissive_texture && !p.emissive_texture->asset.empty());
    const Image glow = load_image(content / p.emissive_texture->asset);
    const auto linear = [](u8 c) {
        const float v = c / 255.0f;
        return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
    };
    const float red = 0.214041f; // Color(0.5) in linear light
    const float peak = linear(47) + red;
    CHECK(std::abs(*p.emissive - 20.0f * peak) < 0.05f);
    CHECK(glow.rgba[7 * 4] == 255 && glow.rgba[7 * 4 + 1] < glow.rgba[7 * 4]); // the brightest, red
    CHECK(has_note(m, "backlight_enabled") && has_note(m, "emission in nits"));
    CHECK(m.written.size() == 3 && fs::exists(m.file));
    // The file kin reads.
    const auto read_back = load_materials3d_asset(m.file);
    CHECK(read_back && read_back->base.material.metallic == p.metallic);
}

void orm_and_refusals() {
    Scratch scratch;
    const fs::path project = scratch.path / "game";
    write(project / "project.godot", "[application]\n");
    CHECK(save_image(picture(255, 128, 64), project / "orm.png"));
    write(project / "orm.tres", R"([gd_resource type="ORMMaterial3D" format=3]
[ext_resource type="Texture2D" path="res://orm.png" id="1"]
[sub_resource type="NoiseTexture2D" id="noise"]
[resource]
orm_texture = ExtResource("1")
ao_enabled = true
emission_enabled = true
emission = Color(0, 0.2, 0, 1)
detail_albedo = SubResource("noise")
uv1_scale = Vector3(2, 2, 1)
)");
    // Outside the content: the map is copied in.
    const fs::path content = scratch.path / "content";
    const auto orm = import_godot_material(project / "orm.tres",
                                           {.project = project, .content = content, .out = content / "materials"});
    CHECK(orm.ok());
    const auto& p = orm->asset.base.material;
    CHECK(p.metallic_roughness && p.occlusion && p.metallic_roughness->asset == p.occlusion->asset);
    CHECK(fs::exists(content / p.metallic_roughness->asset));
    CHECK(*p.emissive == 1.0f && p.emission_color->g > 0.0f && p.emissive_texture->asset.empty()); // no units: x1
    CHECK(has_note(*orm, "detail_albedo") && has_note(*orm, "uv1_scale"));
    // A dry run writes nothing.
    const auto dry = import_godot_material(
        project / "orm.tres", {.project = project, .content = content, .out = content / "dry", .write = false});
    CHECK(dry.ok() && !fs::exists(content / "dry"));

    write(project / "env.tres", "[gd_resource type=\"Environment\" format=3]\n[resource]\nbackground_mode = 2\n");
    CHECK(!import_godot_material(project / "env.tres", {.project = project, .content = content, .out = content}).ok());
    write(project / "broken.tres", "[gd_resource type=\"StandardMaterial3D\" format=3]\n[resource]\nroughness = \n");
    CHECK(!import_godot_material(project / "broken.tres", {.project = project, .content = content, .out = content})
               .ok());
    write(project / "missing.tres", "[gd_resource type=\"StandardMaterial3D\" format=3]\n"
                                    "[ext_resource type=\"Texture2D\" path=\"res://gone.png\" id=\"1\"]\n"
                                    "[resource]\nalbedo_texture = ExtResource(\"1\")\n");
    const auto missing =
        import_godot_material(project / "missing.tres", {.project = project, .content = content, .out = content});
    CHECK(missing.ok() && has_note(*missing, "albedo_texture") && missing->asset.base.material.texture->asset.empty());
}

// The writer's text reads back as the asset it was made from.
void writer_round_trip() {
    MaterialsAsset3D asset;
    auto& p = asset.base.material;
    p.color = Color{1, 2, 3, 4};
    p.roughness = 0.73f;
    p.blend = Blend3D::Additive;
    p.emission_mode = EmissionMode3D::Independent;
    p.emission_color = LinearRgb{0.5f, 0.25f, 2.0f};
    p.texture = MaterialTexture3D{.asset = "a/b.png", .nearest = true};
    p.normal = MaterialTexture3D{};
    asset.base.parts.push_back({.name = "Canvas", .material = {.double_sided = true}});
    asset.variants["wet"].material.roughness = 0.2f;
    const std::string text = write_materials3d_asset(asset);
    CHECK(text.find("0.73,") != std::string::npos); // as written, not 0.7300000190734863
    const auto back = parse_materials3d_asset(text);
    CHECK(back.ok());
    const auto& q = back->base.material;
    CHECK(q.color == p.color && q.roughness == p.roughness && q.blend == p.blend && q.emission_color->b == 2.0f);
    CHECK(q.texture->asset == "a/b.png" && q.texture->nearest && q.normal && q.normal->asset.empty());
    CHECK(back->base.parts.size() == 1 && back->base.parts[0].name == "Canvas" &&
          *back->base.parts[0].material.double_sided);
    CHECK(back->variants.at("wet").material.roughness == 0.2f);
}

void corpus() {
    const char* project = std::getenv("KIN_TEST_GODOT_PROJECT");
    if (!project)
        return;
    int count = 0, failed = 0;
    for (const auto& entry : fs::recursive_directory_iterator(project)) {
        if (entry.path().extension() != ".tres")
            continue;
        std::ifstream in(entry.path());
        std::string first;
        std::getline(in, first);
        in.close();
        if (first.find("StandardMaterial3D") == std::string::npos && first.find("ORMMaterial3D") == std::string::npos)
            continue;
        ++count;
        const auto r = import_godot_material(entry.path(), {.project = project, .content = project,
                                                             .out = fs::path(project) / "kin", .write = false});
        if (!r) {
            ++failed;
            std::cerr << r.diagnostics.front() << '\n';
        }
    }
    std::cout << "corpus: " << count - failed << " of " << count << " imported\n";
    CHECK(count > 0 && failed == 0);
}

} // namespace

int main() {
    standard_material();
    orm_and_refusals();
    writer_round_trip();
    corpus();
    std::cout << "material import tests passed\n";
    return 0;
}
