"""Materials of the shapes kin_import_material unreal reads, made in a scratch Unreal project and exported by
unreal_export.py: the fixtures of tests/fixtures/unreal (kin_material_import3d_tests). Run inside Unreal's editor:

    UnrealEditor-Cmd <Scratch.uproject> -run=pythonscript -EnablePlugins=PythonScriptPlugin \
        -script="<this file> --out /abs/export/folder" -unattended -nosplash -nullrhi

The project gets /Game/Probe (textures imported from PNGs made here, materials and an instance), unsaved.
"""
import os
import runpy
import struct
import sys
import tempfile
import zlib

import unreal

MEL = unreal.MaterialEditingLibrary
TOOLS = unreal.AssetToolsHelpers.get_asset_tools()
P = unreal.MaterialProperty


def png(path, w, h, pixel):
    """An RGBA PNG of `pixel(x, y)` -> (r, g, b, a)."""
    rows = b"".join(b"\0" + b"".join(bytes(pixel(x, y)) for x in range(w)) for y in range(h))
    chunk = lambda t, d: struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def texture(folder, name, pixel, srgb=True, normal=False, flip=False):
    file = os.path.join(folder, name + ".png")
    png(file, 4, 4, pixel)
    task = unreal.AssetImportTask()
    task.filename = file
    task.destination_path = "/Game/Probe/Textures"
    task.destination_name = name
    task.automated = True
    task.save = False
    task.replace_existing = True
    TOOLS.import_asset_tasks([task])
    tex = unreal.load_asset("/Game/Probe/Textures/" + name)
    tex.set_editor_property("srgb", srgb)
    if normal:
        tex.set_editor_property("compression_settings", unreal.TextureCompressionSettings.TC_NORMALMAP)
    tex.set_editor_property("flip_green_channel", flip)
    return tex


def material(name, blend=None, two_sided=False, unlit=False, clip=None):
    m = TOOLS.create_asset(name, "/Game/Probe", unreal.Material, unreal.MaterialFactoryNew())
    if blend is not None:
        m.set_editor_property("blend_mode", blend)
    m.set_editor_property("two_sided", two_sided)
    if unlit:
        m.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    if clip is not None:
        m.set_editor_property("opacity_mask_clip_value", clip)
    return m


def node(m, cls, y=0, **props):
    e = MEL.create_material_expression(m, cls, -400, y)
    for k, v in props.items():
        e.set_editor_property(k, v)
    return e


def main():
    out = sys.argv[sys.argv.index("--out") + 1]
    folder = tempfile.mkdtemp()
    albedo = texture(folder, "T_Albedo", lambda x, y: (40 * x, 60 * y, 200, 255))
    cutout = texture(folder, "T_Cutout", lambda x, y: (200, 100, 50, 255 if x < 2 else 0))
    orm = texture(folder, "T_ORM", lambda x, y: (250, 30 * x + 10, 60 * y, 255), srgb=False)
    gloss = texture(folder, "T_Gloss", lambda x, y: (0, 0, 0, 64 * x), srgb=False)
    normal_dx = texture(folder, "T_NormalDX", lambda x, y: (128 + 20 * x, 100, 240, 255), srgb=False, normal=True)

    # An ORM material, tiled; its base colour a tinted map; the normal map DirectX's; and an instance of it.
    m = material("M_ORM")
    uv = node(m, unreal.MaterialExpressionTextureCoordinate, u_tiling=2.0, v_tiling=3.0)
    base = node(m, unreal.MaterialExpressionTextureSampleParameter2D, parameter_name="Albedo", texture=albedo)
    tint = node(m, unreal.MaterialExpressionVectorParameter, parameter_name="Tint",
                default_value=unreal.LinearColor(1.0, 0.5, 0.25, 1.0))
    mul = node(m, unreal.MaterialExpressionMultiply)
    MEL.connect_material_expressions(base, "RGB", mul, "A")
    MEL.connect_material_expressions(tint, "", mul, "B")
    MEL.connect_material_property(mul, "", P.MP_BASE_COLOR)
    o = node(m, unreal.MaterialExpressionTextureSample, texture=orm)
    o.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
    MEL.connect_material_property(o, "R", P.MP_AMBIENT_OCCLUSION)
    MEL.connect_material_property(o, "G", P.MP_ROUGHNESS)
    MEL.connect_material_property(o, "B", P.MP_METALLIC)
    n = node(m, unreal.MaterialExpressionTextureSample, texture=normal_dx)
    n.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL)
    MEL.connect_material_property(n, "RGB", P.MP_NORMAL)
    for s in (base, o, n):
        MEL.connect_material_expressions(uv, "", s, "UVs")
    MEL.recompile_material(m)
    mi = TOOLS.create_asset("MI_ORM_Green", "/Game/Probe", unreal.MaterialInstanceConstant,
                            unreal.MaterialInstanceConstantFactoryNew())
    MEL.set_material_instance_parent(mi, m)
    MEL.set_material_instance_vector_parameter_value(mi, "Tint", unreal.LinearColor(0.0, 1.0, 0.0, 1.0))

    # Roughness as one minus a gloss map's alpha; a constant metallic and colour.
    m = material("M_Gloss")
    g = node(m, unreal.MaterialExpressionTextureSample, texture=gloss)
    g.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
    inv = node(m, unreal.MaterialExpressionOneMinus)
    MEL.connect_material_expressions(g, "A", inv, "")
    MEL.connect_material_property(inv, "", P.MP_ROUGHNESS)
    MEL.connect_material_property(node(m, unreal.MaterialExpressionConstant, r=0.8), "", P.MP_METALLIC)
    MEL.connect_material_property(node(m, unreal.MaterialExpressionConstant3Vector,
                                       constant=unreal.LinearColor(0.2, 0.4, 0.6, 1)), "", P.MP_BASE_COLOR)
    MEL.recompile_material(m)

    # A two-sided cut-out by its base map's alpha.
    m = material("M_Cutout", blend=unreal.BlendMode.BLEND_MASKED, two_sided=True, clip=0.4)
    c = node(m, unreal.MaterialExpressionTextureSample, texture=cutout)
    MEL.connect_material_property(c, "RGB", P.MP_BASE_COLOR)
    MEL.connect_material_property(c, "A", P.MP_OPACITY_MASK)
    MEL.recompile_material(m)

    # Additive glow: a colour times a strength.
    m = material("M_Glow", blend=unreal.BlendMode.BLEND_ADDITIVE)
    k = node(m, unreal.MaterialExpressionConstant3Vector, constant=unreal.LinearColor(1.0, 0.5, 0.0, 1))
    s = node(m, unreal.MaterialExpressionScalarParameter, parameter_name="Strength", default_value=5.0)
    mul = node(m, unreal.MaterialExpressionMultiply)
    MEL.connect_material_expressions(k, "", mul, "A")
    MEL.connect_material_expressions(s, "", mul, "B")
    MEL.connect_material_property(mul, "", P.MP_EMISSIVE_COLOR)
    MEL.recompile_material(m)

    # Unlit: its emissive is its colour.
    m = material("M_Unlit", unlit=True)
    MEL.connect_material_property(node(m, unreal.MaterialExpressionTextureSample, texture=albedo), "RGB",
                                  P.MP_EMISSIVE_COLOR)
    MEL.recompile_material(m)

    # A blend of a map and a colour by a varying alpha: not read (noted).
    m = material("M_Lerp")
    a = node(m, unreal.MaterialExpressionTextureSample, texture=albedo)
    lerp = node(m, unreal.MaterialExpressionLinearInterpolate)
    MEL.connect_material_expressions(a, "RGB", lerp, "A")
    MEL.connect_material_expressions(node(m, unreal.MaterialExpressionScalarParameter, parameter_name="Mix",
                                          default_value=0.5), "", lerp, "Alpha")
    MEL.connect_material_property(lerp, "", P.MP_BASE_COLOR)
    MEL.recompile_material(m)

    # A graph of many node types (Tier 2: kin_import_material's graph import).
    m = material("M_GraphMix")
    uv = node(m, unreal.MaterialExpressionTextureCoordinate, u_tiling=4.0, v_tiling=4.0)
    pan = node(m, unreal.MaterialExpressionPanner, speed_x=0.1, speed_y=0.0)
    MEL.connect_material_expressions(uv, "", pan, "Coordinate")
    tex = node(m, unreal.MaterialExpressionTextureSampleParameter2D, parameter_name="Albedo", texture=albedo)
    MEL.connect_material_expressions(pan, "", tex, "UVs")
    tint = node(m, unreal.MaterialExpressionVectorParameter, parameter_name="Tint",
                default_value=unreal.LinearColor(0.9, 0.3, 0.1, 1.0))
    fres = node(m, unreal.MaterialExpressionFresnel, exponent=3.0)
    lerp = node(m, unreal.MaterialExpressionLinearInterpolate)
    MEL.connect_material_expressions(tex, "RGB", lerp, "A")
    MEL.connect_material_expressions(tint, "", lerp, "B")
    MEL.connect_material_expressions(fres, "", lerp, "Alpha")
    vc = node(m, unreal.MaterialExpressionVertexColor)
    mul = node(m, unreal.MaterialExpressionMultiply)
    MEL.connect_material_expressions(lerp, "", mul, "A")
    MEL.connect_material_expressions(vc, "", mul, "B")
    MEL.connect_material_property(mul, "", P.MP_BASE_COLOR)
    # Roughness: 1 - clamp(power(the map's G, 2), 0.1, 0.9).
    o = node(m, unreal.MaterialExpressionTextureSample, texture=orm)
    o.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
    powr = node(m, unreal.MaterialExpressionPower, const_exponent=2.0)
    MEL.connect_material_expressions(o, "G", powr, "Base")
    clamp = node(m, unreal.MaterialExpressionClamp, min_default=0.1, max_default=0.9)
    MEL.connect_material_expressions(powr, "", clamp, "")
    inv = node(m, unreal.MaterialExpressionOneMinus)
    MEL.connect_material_expressions(clamp, "", inv, "")
    MEL.connect_material_property(inv, "", P.MP_ROUGHNESS)
    # Emission: (sin(time) * 0.5 + 0.5) times a colour appended from a mask of the world position.
    t = node(m, unreal.MaterialExpressionTime)
    sine = node(m, unreal.MaterialExpressionSine, period=1.0)
    MEL.connect_material_expressions(t, "", sine, "")
    wp = node(m, unreal.MaterialExpressionWorldPosition)
    mask = node(m, unreal.MaterialExpressionComponentMask, r=True, g=True, b=False, a=False)
    MEL.connect_material_expressions(wp, "", mask, "")
    app = node(m, unreal.MaterialExpressionAppendVector)
    MEL.connect_material_expressions(mask, "", app, "A")
    MEL.connect_material_expressions(sine, "", app, "B")
    MEL.connect_material_property(app, "", P.MP_EMISSIVE_COLOR)
    # Normal: the DirectX map, flattened half way to +z.
    nm = node(m, unreal.MaterialExpressionTextureSample, texture=normal_dx)
    nm.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL)
    flat = node(m, unreal.MaterialExpressionLinearInterpolate, const_alpha=0.5)
    MEL.connect_material_expressions(nm, "RGB", flat, "A")
    up = node(m, unreal.MaterialExpressionConstant3Vector, constant=unreal.LinearColor(0, 0, 1, 1))
    MEL.connect_material_expressions(up, "", flat, "B")
    MEL.connect_material_property(flat, "", P.MP_NORMAL)
    MEL.recompile_material(m)
    mi = TOOLS.create_asset("MI_GraphMix_Blue", "/Game/Probe", unreal.MaterialInstanceConstant,
                            unreal.MaterialInstanceConstantFactoryNew())
    MEL.set_material_instance_parent(mi, m)
    MEL.set_material_instance_vector_parameter_value(mi, "Tint", unreal.LinearColor(0.1, 0.2, 0.9, 1.0))

    # A Custom (HLSL) node: noted, its output left at kin's default.
    m = material("M_Custom")
    c = node(m, unreal.MaterialExpressionCustom, code="return float3(1, 0, 0);")
    MEL.connect_material_property(c, "", P.MP_BASE_COLOR)
    MEL.recompile_material(m)

    exporter = os.path.join(os.path.dirname(os.path.abspath(__file__)), "unreal_export.py")
    sys.argv = [exporter, "--content", "/Game/Probe", "--out", out]
    runpy.run_path(exporter, run_name="__main__")


main()
