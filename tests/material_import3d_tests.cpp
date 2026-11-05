// Other engines' materials as kin's (kin/render3d/material_import3d.hpp), on the CPU: Godot's StandardMaterial3D and
// ORMMaterial3D read into kin.materials3d/1 with the textures kin needs (metallic and roughness channels packed into
// one map, occlusion moved to R, an added emission colour put into its map); what kin can't show is noted; files that
// aren't such materials are refused. With KIN_TEST_GODOT_PROJECT (a Godot project folder), every material of it is
// imported (a dry run); with KIN_TEST_UNITY_PROJECT, every .mat of a Unity project. Unity's .mat: URP Lit, Standard,
// HDRP Lit, their maps packed, tiling, emission, surfaces; TIFF and PSD pictures decoded.
#include <kin/assets/image.hpp>
#include <kin/render3d/material_asset3d.hpp>
#include <kin/render3d/material_import3d.hpp>

#include "image_decode3d.hpp"

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
    CHECK(has_note(*orm, "detail_albedo") && !has_note(*orm, "uv1_scale"));
    CHECK(p.uv_scale->x == 2.0f && p.uv_scale->y == 2.0f && p.uv_offset->x == 0.0f);
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
    p.uv_scale = Vec2f{2.0f, 0.5f};
    p.uv_offset = Vec2f{0.25f, -1.0f};
    asset.base.parts.push_back({.name = "Canvas", .material = {.double_sided = true}});
    asset.variants["wet"].material.roughness = 0.2f;
    const std::string text = write_materials3d_asset(asset);
    CHECK(text.find("0.73,") != std::string::npos); // as written, not 0.7300000190734863
    const auto back = parse_materials3d_asset(text);
    CHECK(back.ok());
    const auto& q = back->base.material;
    CHECK(q.color == p.color && q.roughness == p.roughness && q.blend == p.blend && q.emission_color->b == 2.0f);
    CHECK(q.texture->asset == "a/b.png" && q.texture->nearest && q.normal && q.normal->asset.empty());
    CHECK(q.uv_scale == p.uv_scale && q.uv_offset == p.uv_offset);
    CHECK(back->base.parts.size() == 1 && back->base.parts[0].name == "Canvas" &&
          *back->base.parts[0].material.double_sided);
    CHECK(back->variants.at("wet").material.roughness == 0.2f);
}

// A Unity project's asset: the file and its .meta with its GUID.
void unity_asset(const fs::path& file, const std::string& guid) {
    write(fs::path(file.string() + ".meta"), "fileFormatVersion: 2\nguid: " + guid + "\nTextureImporter:\n");
}

std::string unity_mat(const std::string& name, const std::string& shader, const std::string& textures,
                      const std::string& floats, const std::string& colors, const std::string& keywords = "") {
    return "%YAML 1.1\n%TAG !u! tag:unity3d.com,2011:\n--- !u!21 &2100000\nMaterial:\n  serializedVersion: 6\n"
           "  m_Name: " + name + "\n  m_Shader: " + shader + "\n  m_ShaderKeywords: " + keywords +
           "\n  m_SavedProperties:\n    serializedVersion: 3\n    m_TexEnvs:\n" + textures + "    m_Floats:\n" + floats +
           "    m_Colors:\n" + colors + "--- !u!114 &1\nMonoBehaviour:\n  m_Name: \n";
}
std::string tex(const std::string& key, const std::string& guid, std::string scale = "{x: 1, y: 1}",
                std::string offset = "{x: 0, y: 0}") {
    return "    - " + key + ":\n        m_Texture: {fileID: 2800000, guid: " + guid + ", type: 3}\n        m_Scale: " +
           scale + "\n        m_Offset: " + offset + "\n";
}

void unity_materials() {
    Scratch scratch;
    const fs::path project = scratch.path / "unity", content = scratch.path;
    const fs::path textures = project / "Assets/Textures";
    CHECK(save_image(picture(10, 20, 30), textures / "albedo.png"));
    unity_asset(textures / "albedo.png", "a0000000000000000000000000000001");
    CHECK(save_image(picture(100, 0, 0, 200), textures / "metal.png")); // metallic R, smoothness A
    unity_asset(textures / "metal.png", "a0000000000000000000000000000002");
    CHECK(save_image(picture(0, 120, 0), textures / "occlusion.png")); // Unity's occlusion: G
    unity_asset(textures / "occlusion.png", "a0000000000000000000000000000003");
    CHECK(save_image(picture(128, 128, 255), textures / "normal.png"));
    unity_asset(textures / "normal.png", "a0000000000000000000000000000004");
    CHECK(save_image(picture(60, 0, 0, 100), textures / "mask.png")); // HDRP: R metal, G AO, A smoothness
    unity_asset(textures / "mask.png", "a0000000000000000000000000000005");
    fs::copy_file(fs::path(KIN_TEST_IMAGES) / "rgba_lzw_predictor.tif", textures / "tiled.tif");
    unity_asset(textures / "tiled.tif", "a0000000000000000000000000000006");
    write(project / "Assets/Shaders/Water.shadergraph", "{}");
    unity_asset(project / "Assets/Shaders/Water.shadergraph", "b0000000000000000000000000000001");
    const MaterialImportOptions3D options{.project = project, .content = content, .out = content / "kin"};

    // URP Lit: tiling, the metallic map's alpha as smoothness, occlusion from G, emission, a double-sided cut-out.
    write(project / "Assets/Lit.mat",
          unity_mat("Brass", "{fileID: 4800000, guid: 933532a4fcc9baf4fa0491de14d08ed7, type: 3}",
                    tex("_BaseMap", "a0000000000000000000000000000001", "{x: 2, y: 3}", "{x: 0.5, y: 0}") +
                        tex("_MetallicGlossMap", "a0000000000000000000000000000002") +
                        tex("_OcclusionMap", "a0000000000000000000000000000003") +
                        tex("_BumpMap", "a0000000000000000000000000000004") +
                        "    - _EmissionMap:\n        m_Texture: {fileID: 0}\n        m_Scale: {x: 1, y: 1}\n"
                        "        m_Offset: {x: 0, y: 0}\n",
                    "    - _Smoothness: 0.5\n    - _Metallic: 0.9\n    - _SmoothnessTextureChannel: 0\n"
                    "    - _BumpScale: 0.8\n    - _OcclusionStrength: 0.7\n    - _Surface: 0\n    - _AlphaClip: 1\n"
                    "    - _Cutoff: 0.4\n    - _Cull: 0\n    - _WorkflowMode: 1\n",
                    "    - _BaseColor: {r: 1, g: 0.5, b: 0.25, a: 1}\n    - _EmissionColor: {r: 2, g: 1, b: 0, a: 1}\n",
                    "_EMISSION _METALLICSPECGLOSSMAP _NORMALMAP"));
    const auto lit = import_unity_material(project / "Assets/Lit.mat", options);
    if (!lit) {
        for (const auto& d : lit.diagnostics)
            std::cerr << d << '\n';
        CHECK(false);
    }
    const auto& p = lit->asset.base.material;
    CHECK(lit->name == "Brass" && lit->notes.empty());
    CHECK(p.color->r == 255 && p.color->g == 128 && p.color->b == 64);
    CHECK(p.texture->asset == fs::path("unity/Assets/Textures/albedo.png"));
    CHECK(*p.uv_scale == (Vec2f{2, 3}) && *p.uv_offset == (Vec2f{0.5f, 0}));
    CHECK(*p.metallic == 1.0f && *p.roughness == 1.0f); // both in the map
    const Image mr = load_image(content / p.metallic_roughness->asset);
    for (int i = 0; i < 8; ++i) {
        const float smooth = (200 / 255.0f) * 0.5f;
        CHECK(std::abs(int(mr.rgba[i * 4 + 1]) - int(std::lround((1.0f - smooth) * 255))) <= 1);
        CHECK(mr.rgba[i * 4 + 2] == 100 + i);
    }
    const Image ao = load_image(content / p.occlusion->asset);
    CHECK(ao.rgba[3 * 4] == 123 && std::abs(*p.occlusion_strength - 0.7f) < 1e-6f);
    CHECK(std::abs(*p.normal_scale - 0.8f) < 1e-6f && p.normal->asset == fs::path("unity/Assets/Textures/normal.png"));
    CHECK(*p.blend == Blend3D::Mask && std::abs(*p.alpha_cutoff - 0.4f) < 1e-6f && *p.double_sided);
    CHECK(*p.emissive == 1.0f && std::abs(p.emission_color->r - 4.9538f) < 0.01f && p.emission_color->b == 0.0f);

    // Standard (built-in), fade: blended; no map: roughness 1 - glossiness.
    write(project / "Assets/Standard.mat",
          unity_mat("Glass", "{fileID: 46, guid: 0000000000000000f000000000000000, type: 0}",
                    tex("_MainTex", "a0000000000000000000000000000001"),
                    "    - _Mode: 2\n    - _Glossiness: 0.8\n    - _Metallic: 0.3\n",
                    "    - _Color: {r: 1, g: 1, b: 1, a: 0.5}\n"));
    const auto standard = import_unity_material(project / "Assets/Standard.mat", options);
    CHECK(standard.ok());
    const auto& q = standard->asset.base.material;
    CHECK(*q.blend == Blend3D::Alpha && std::abs(*q.roughness - 0.2f) < 1e-6f && std::abs(*q.metallic - 0.3f) < 1e-6f);
    CHECK(q.color->a == 128 && q.metallic_roughness->asset.empty());

    // HDRP Lit: the mask map remapped; an additive transparent surface.
    write(project / "Assets/Hdrp.mat",
          unity_mat("Panel", "{fileID: 4800000, guid: 6e4ae4064600d784cac1e41a9e6f2e59, type: 3}",
                    tex("_BaseColorMap", "a0000000000000000000000000000001") +
                        tex("_MaskMap", "a0000000000000000000000000000005"),
                    "    - _SmoothnessRemapMin: 0.2\n    - _SmoothnessRemapMax: 0.8\n    - _SurfaceType: 1\n"
                    "    - _BlendMode: 1\n    - _DoubleSidedEnable: 1\n",
                    "    - _BaseColor: {r: 1, g: 1, b: 1, a: 1}\n    - _EmissiveColor: {r: 0, g: 0, b: 0, a: 1}\n"));
    const auto hdrp = import_unity_material(project / "Assets/Hdrp.mat", options);
    CHECK(hdrp.ok());
    const auto& h = hdrp->asset.base.material;
    CHECK(*h.blend == Blend3D::Additive && *h.double_sided && *h.emissive == 0.0f);
    const Image mask = load_image(content / h.metallic_roughness->asset);
    CHECK(std::abs(int(mask.rgba[1]) - int(std::lround((1.0f - (0.2f + 0.6f * 100 / 255.0f)) * 255))) <= 1);
    CHECK(mask.rgba[2] == 60 && h.occlusion && !h.occlusion->asset.empty());

    // A Shader Graph's material, a TIFF base map (decoded into a PNG), a texture whose file is gone.
    write(project / "Assets/Water.mat",
          unity_mat("Water", "{fileID: -6465566751694194690, guid: b0000000000000000000000000000001, type: 3}",
                    tex("_BaseMap", "a0000000000000000000000000000006") +
                        tex("_BumpMap", "c0000000000000000000000000000009"),
                    "    - _Smoothness: 0.9\n", "    - _BaseColor: {r: 1, g: 1, b: 1, a: 1}\n", "BOOLEAN_ABC_ON"));
    const auto water = import_unity_material(project / "Assets/Water.mat", options);
    CHECK(water.ok() && has_note(*water, "a Shader Graph (Water.shadergraph)") && has_note(*water, "_BumpMap"));
    CHECK(!has_note(*water, "keyword")); // a graph's keywords are its own
    const Image tiled = load_image(content / water->asset.base.material.texture->asset);
    CHECK(tiled.size == (Vec2i{5, 3}) && tiled.rgba[(2 * 5 + 4) * 4] == 200 && tiled.rgba[(2 * 5 + 4) * 4 + 3] == 208);

    write(project / "Assets/NotOne.mat", "%YAML 1.1\n--- !u!28 &1\nTexture2D:\n  m_Name: x\n");
    CHECK(!import_unity_material(project / "Assets/NotOne.mat", options).ok());
}

// TIFF (made by PIL: LZW with the horizontal predictor, Deflate, PackBits, uncompressed, 16-bit grey) and PSD (made here)
// as 8-bit RGBA.
void pictures() {
    const fs::path dir = KIN_TEST_IMAGES;
    for (const char* name : {"rgba_lzw_predictor.tif", "rgba_deflate.tif", "rgba_packbits.tif", "rgb_raw.tif"}) {
        std::ifstream in(dir / name, std::ios::binary);
        const std::string bytes{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
        std::string why;
        const auto image =
            material_import::decode_extra_image({reinterpret_cast<const u8*>(bytes.data()), bytes.size()}, &why);
        if (!image) {
            std::cerr << name << ": " << why << '\n';
            CHECK(false);
        }
        CHECK(image->size == (Vec2i{5, 3}));
        const bool alpha = std::string_view(name).rfind("rgba", 0) == 0;
        for (int y = 0; y < 3; ++y)
            for (int x = 0; x < 5; ++x) {
                const u8* p = &image->rgba[(y * 5 + x) * 4];
                CHECK(p[0] == x * 50 && p[1] == y * 100 && p[2] == 255 - x * 40 && p[3] == (alpha ? 128 + y * 40 : 255));
            }
    }
    {
        std::ifstream in(dir / "grey16_lzw.tif", std::ios::binary);
        const std::string bytes{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
        const auto image = material_import::decode_extra_image({reinterpret_cast<const u8*>(bytes.data()), bytes.size()});
        CHECK(image && image->size == (Vec2i{5, 3}));
        for (int y = 0; y < 3; ++y)
            for (int x = 0; x < 5; ++x) {
                const int want = int(std::lround((x * 13000 + y * 700) / 65535.0 * 255));
                CHECK(std::abs(int(image->rgba[(y * 5 + x) * 4]) - want) <= 1);
            }
    }
    // A PSD of 2 x 2, RGB, raw: its composited picture.
    std::string psd = "8BPS";
    const auto u16be = [&](int v) { psd += char(v >> 8), psd += char(v & 255); };
    const auto u32be = [&](int v) { u16be(v >> 16), u16be(v & 0xffff); };
    u16be(1), psd += std::string(6, '\0'), u16be(3), u32be(2), u32be(2), u16be(8), u16be(3);
    u32be(0), u32be(0), u32be(0), u16be(0); // no colour data, resources, layers; raw
    for (int c = 0; c < 3; ++c)
        for (int i = 0; i < 4; ++i)
            psd += char(c * 80 + i * 10);
    const auto image = material_import::decode_extra_image({reinterpret_cast<const u8*>(psd.data()), psd.size()});
    CHECK(image && image->size == (Vec2i{2, 2}) && image->rgba[3 * 4 + 1] == 110 && image->rgba[3 * 4 + 3] == 255);
    CHECK(!material_import::decode_extra_image({reinterpret_cast<const u8*>("garbage!"), 8}));
}

void unity_corpus() {
    const char* project = std::getenv("KIN_TEST_UNITY_PROJECT");
    if (!project)
        return;
    int count = 0, failed = 0;
    for (const auto& entry : fs::recursive_directory_iterator(fs::path(project) / "Assets")) {
        if (entry.path().extension() != ".mat")
            continue;
        ++count;
        const auto r = import_unity_material(entry.path(), {.project = project, .content = project,
                                                             .out = fs::path(project) / "kin", .write = false});
        if (!r) {
            ++failed;
            std::cerr << r.diagnostics.front() << '\n';
        }
    }
    std::cout << "unity corpus: " << count - failed << " of " << count << " imported\n";
    CHECK(count > 0 && failed == 0);
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
    unity_materials();
    pictures();
    corpus();
    unity_corpus();
    std::cout << "material import tests passed\n";
    return 0;
}
