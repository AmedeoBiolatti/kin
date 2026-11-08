"""The material lineup (lineup.json) in Unreal 5: its textures and meshes imported, its materials made as graphs
(saved under /Game/Lineup, for unreal_export.py and kin_import_material unreal --graphs), its level built and
pictured. Run in the editor (it renders: not -nullrhi):

    LINEUP_DIR=<assets + lineup.json> LINEUP_OUT=<picture.png> \\
        UnrealEditor <Lineup.uproject> -ExecutePythonScript=<this file>

Settings as Godot's and kin's: manual exposure 1 without the physical camera, no tone curve (ToneCurveAmount,
ExpandGamut, BlueCorrection 0: linear light encoded to sRGB), no AA, GI, reflections, AO, bloom or grain (the
project's DefaultEngine.ini), one movable sun of pi lux (Lambert's 1/pi undone: kin's and Godot's sun of 1) without
shadow or source angle, no sky. Coordinates: kin's (x, y, z) metres are Unreal's (-z, x, y) x 100 cm.
"""
import json
import math
import os

import unreal

MEL = unreal.MaterialEditingLibrary
TOOLS = unreal.AssetToolsHelpers.get_asset_tools()
P = unreal.MaterialProperty
ROOT = "/Game/Lineup"
DIR = os.environ["LINEUP_DIR"]
OUT = os.environ["LINEUP_OUT"]
SCENE = json.load(open(os.path.join(DIR, "lineup.json")))
# Interchange's glTF import swaps glTF's y and z (glTF +z, the meshes' front, becomes Unreal's +Y): a quarter turn
# about Z brings glTF's +x to Unreal's +Y (kin's right) and +z to -X (toward the camera). Roll, pitch, yaw.
MESH_ROT = [float(v) for v in os.environ.get("LINEUP_MESH_ROT", "0,0,90").split(",")]


def log(*a):
    unreal.log("lineup: " + " ".join(str(x) for x in a))


def ue(p, scale=100.0):
    return unreal.Vector(-p[2] * scale, p[0] * scale, p[1] * scale)


def import_file(name, dest):
    task = unreal.AssetImportTask()
    task.filename = os.path.join(DIR, name)
    task.destination_path = dest
    task.automated = True
    task.save = True
    task.replace_existing = True
    TOOLS.import_asset_tasks([task])
    return [unreal.load_asset(p) for p in task.get_editor_property("imported_object_paths")]


def texture(name, srgb, normal=False):
    t = [a for a in import_file(name + ".png", ROOT + "/Textures") if isinstance(a, unreal.Texture2D)][0]
    t.set_editor_property("srgb", srgb)
    t.set_editor_property("compression_settings", unreal.TextureCompressionSettings.TC_NORMALMAP if normal
                          else unreal.TextureCompressionSettings.TC_VECTOR_DISPLACEMENTMAP)  # uncompressed 8-bit
    t.set_editor_property("flip_green_channel", normal)  # our map is OpenGL's; Unreal's are DirectX's
    unreal.EditorAssetLibrary.save_loaded_asset(t)
    return t


def mesh(name):
    meshes = [a for a in import_file(name + ".glb", ROOT + "/Meshes/" + name) if isinstance(a, unreal.StaticMesh)]
    if not meshes:  # Interchange may place it in a sub-folder
        for p in unreal.EditorAssetLibrary.list_assets(ROOT + "/Meshes/" + name, recursive=True):
            a = unreal.load_asset(p)
            if isinstance(a, unreal.StaticMesh):
                meshes.append(a)
    return meshes[0]


def material(name, unlit=False):
    path = ROOT + "/Materials/" + name
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        unreal.EditorAssetLibrary.delete_asset(path)
    m = TOOLS.create_asset(name, ROOT + "/Materials", unreal.Material, unreal.MaterialFactoryNew())
    if unlit:
        m.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    return m


def node(m, cls, **props):
    e = MEL.create_material_expression(m, cls, -400, 0)
    for k, v in props.items():
        e.set_editor_property(k, v)
    return e


def link(a, a_pin, b, b_pin):
    if not MEL.connect_material_expressions(a, a_pin, b, b_pin):
        log("connect failed", a, a_pin, b, b_pin)


def mask(m, x, *channels):
    k = node(m, unreal.MaterialExpressionComponentMask, **{c: c in channels for c in "rgba"})  # (R and G default on)
    link(x, "", k, "")
    return k


def mul(m, a, b=None, const=None, a_pin=""):
    e = node(m, unreal.MaterialExpressionMultiply, **({"const_b": const} if const is not None else {}))
    link(a, a_pin, e, "A")
    if b is not None:
        link(b, "", e, "B")
    return e


def add(m, a, b=None, const=None):
    e = node(m, unreal.MaterialExpressionAdd, **({"const_b": const} if const is not None else {}))
    link(a, "", e, "A")
    if b is not None:
        link(b, "", e, "B")
    return e


def append(m, a, b):
    e = node(m, unreal.MaterialExpressionAppendVector)
    link(a, "", e, "A")
    link(b, "", e, "B")
    return e


def vec3(m, c):
    return node(m, unreal.MaterialExpressionConstant3Vector, constant=unreal.LinearColor(c[0], c[1], c[2], 1))


def const(m, v):
    return node(m, unreal.MaterialExpressionConstant, r=v)


def materials():
    out = {}
    checker = texture("checker", True)
    data = texture("mask", False)
    normal = texture("normal", False, normal=True)

    m = material("TexTint", unlit=True)
    uv = node(m, unreal.MaterialExpressionTextureCoordinate, u_tiling=3.0, v_tiling=3.0)
    t = node(m, unreal.MaterialExpressionTextureSampleParameter2D, parameter_name="checker_tex", texture=checker)
    link(uv, "", t, "UVs")
    tint = node(m, unreal.MaterialExpressionVectorParameter, parameter_name="Tint",
                default_value=unreal.LinearColor(0.8, 0.5, 0.25, 1))
    MEL.connect_material_property(mul(m, t, tint, a_pin="RGB"), "", P.MP_EMISSIVE_COLOR)
    out["TexTint"] = m

    m = material("UVMath", unlit=True)
    uv = node(m, unreal.MaterialExpressionTextureCoordinate)
    u, v = mask(m, uv, "r"), mask(m, uv, "g")
    fr = node(m, unreal.MaterialExpressionFrac)
    link(mul(m, u, const=4.0), "", fr, "")
    sine = node(m, unreal.MaterialExpressionSine, period=6.2831853)  # sin(x * 2 pi / period)
    link(mul(m, u, const=6.2831853), "", sine, "")
    wave = add(m, mul(m, sine, const=0.5), const=0.5)
    MEL.connect_material_property(append(m, append(m, fr, v), wave), "", P.MP_EMISSIVE_COLOR)
    out["UVMath"] = m

    m = material("LerpSmooth", unlit=True)
    uv = node(m, unreal.MaterialExpressionTextureCoordinate)
    ss = node(m, unreal.MaterialExpressionSmoothStep, const_min=0.3, const_max=0.7)
    link(mask(m, uv, "r"), "", ss, "Value")
    lerp = node(m, unreal.MaterialExpressionLinearInterpolate)
    link(vec3(m, (0.9, 0.1, 0.05)), "", lerp, "A")
    link(vec3(m, (0.05, 0.3, 0.9)), "", lerp, "B")
    link(ss, "", lerp, "Alpha")
    st = node(m, unreal.MaterialExpressionStep, const_y=0.5)
    link(mask(m, uv, "g"), "", st, "X")
    MEL.connect_material_property(add(m, lerp, mul(m, st, const=0.1)), "", P.MP_EMISSIVE_COLOR)
    out["LerpSmooth"] = m

    m = material("DataMask", unlit=True)
    t = node(m, unreal.MaterialExpressionTextureSampleParameter2D, parameter_name="mask_tex", texture=data)
    t.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
    rm = node(m, unreal.MaterialExpressionLinearInterpolate, const_a=0.2, const_b=0.8)  # remap 0..1 to 0.2..0.8
    link(t, "R", rm, "Alpha")
    inv = node(m, unreal.MaterialExpressionOneMinus)
    link(t, "G", inv, "")
    MEL.connect_material_property(append(m, append(m, rm, mul(m, rm, inv)), rm), "", P.MP_EMISSIVE_COLOR)
    out["DataMask"] = m

    for name, base, metal, rough in (("Plain", (0.8, 0.8, 0.8), 0.0, 0.5), ("Gold", (1.0, 0.766, 0.336), 1.0, 0.3),
                                     ("Bumps", (0.6, 0.6, 0.6), 0.0, 0.4), ("Rim", (0.05, 0.1, 0.3), 0.0, 0.6)):
        m = material(name)
        MEL.connect_material_property(vec3(m, base), "", P.MP_BASE_COLOR)
        MEL.connect_material_property(const(m, metal), "", P.MP_METALLIC)
        MEL.connect_material_property(const(m, rough), "", P.MP_ROUGHNESS)
        if name == "Bumps":
            uv = node(m, unreal.MaterialExpressionTextureCoordinate, u_tiling=4.0, v_tiling=4.0)
            n = node(m, unreal.MaterialExpressionTextureSample, texture=normal)
            n.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL)
            link(uv, "", n, "UVs")
            MEL.connect_material_property(n, "RGB", P.MP_NORMAL)
        if name == "Rim":
            f = node(m, unreal.MaterialExpressionFresnel, exponent=3.0, base_reflect_fraction=0.0)
            MEL.connect_material_property(mul(m, f, vec3(m, (1, 0.5, 0.1))), "", P.MP_EMISSIVE_COLOR)
        out[name] = m
    for m in out.values():
        MEL.recompile_material(m)
        unreal.EditorAssetLibrary.save_loaded_asset(m)
    return out


def build():
    mats = materials()
    meshes = {"quad": mesh("quad"), "sphere": mesh("sphere")}
    level = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    # A fresh, empty level (new_level fails on an existing map, leaving the editor's start-up one: a sky, a landscape).
    if unreal.EditorAssetLibrary.does_asset_exist(ROOT + "/LineupMap"):
        unreal.EditorAssetLibrary.delete_asset(ROOT + "/LineupMap")
    if not level.new_level(ROOT + "/LineupMap"):
        raise RuntimeError("cannot make the level " + ROOT + "/LineupMap")
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    for o in SCENE["objects"]:
        a = actors.spawn_actor_from_class(unreal.StaticMeshActor, ue(o["position"]),
                                          unreal.Rotator(MESH_ROT[0], MESH_ROT[1], MESH_ROT[2]))
        c = a.static_mesh_component
        c.set_mobility(unreal.ComponentMobility.MOVABLE)
        c.set_static_mesh(meshes[o["mesh"]])
        c.set_material(0, mats[o["material"]])
        c.set_cast_shadow(False)
    t = SCENE["sun"]["toward"]
    travel = unreal.Vector(t[2], -t[0], -t[1])  # from the light: -(toward) in Unreal's axes
    sun = actors.spawn_actor_from_class(unreal.DirectionalLight, unreal.Vector(0, 0, 500),
                                        unreal.MathLibrary.make_rot_from_x(travel))
    lc = sun.get_component_by_class(unreal.DirectionalLightComponent)
    lc.set_mobility(unreal.ComponentMobility.MOVABLE)
    lc.set_intensity(math.pi * SCENE["sun"]["intensity"])
    lc.set_light_color(unreal.LinearColor(1, 1, 1, 1))
    lc.set_cast_shadows(False)
    lc.set_editor_property("light_source_angle", 0.0)
    pp = actors.spawn_actor_from_class(unreal.PostProcessVolume, unreal.Vector(0, 0, 0))
    pp.set_editor_property("unbound", True)
    s = pp.get_editor_property("settings")
    for key, value in (("auto_exposure_method", unreal.AutoExposureMethod.AEM_MANUAL), ("auto_exposure_bias", 0.0),
                       ("auto_exposure_apply_physical_camera_exposure", False), ("tone_curve_amount", 0.0),
                       ("expand_gamut", 0.0), ("blue_correction", 0.0), ("vignette_intensity", 0.0),
                       ("bloom_intensity", 0.0), ("ambient_occlusion_intensity", 0.0), ("film_grain_intensity", 0.0),
                       ("motion_blur_amount", 0.0),
                       ("dynamic_global_illumination_method", unreal.DynamicGlobalIlluminationMethod.NONE),
                       ("reflection_method", unreal.ReflectionMethod.NONE)):
        s.set_editor_property("override_" + key, True)
        s.set_editor_property(key, value)
    pp.set_editor_property("settings", s)
    cam = SCENE["camera"]
    w, h = SCENE["size"]
    camera = actors.spawn_actor_from_class(unreal.CameraActor, ue(cam["eye"]), unreal.Rotator(0, 0, 0))
    cc = camera.camera_component
    cc.set_editor_property("field_of_view",
                           math.degrees(2 * math.atan(math.tan(math.radians(cam["fov_y"]) / 2) * w / h)))
    cc.set_editor_property("constrain_aspect_ratio", True)
    cc.set_editor_property("aspect_ratio", w / h)
    level.save_current_level()
    return camera


state = {"tick": 0, "camera": None, "task": None, "done_at": None}


def tick(_dt):
    state["tick"] += 1
    n = state["tick"]
    try:
        if n == 60:
            state["camera"] = build()
            log("built")
        elif n == 120:
            unreal.AutomationLibrary.finish_loading_before_screenshot()
        elif n == 300:
            w, h = SCENE["size"]
            state["task"] = unreal.AutomationLibrary.take_high_res_screenshot(w, h, OUT, state["camera"])
            log("screenshot requested", OUT)
        elif state["task"] is not None and state["done_at"] is None and state["task"].is_task_done():
            state["done_at"] = n
        elif state["done_at"] is not None and n > state["done_at"] + 30:
            log("done", os.path.exists(OUT))
            unreal.unregister_slate_post_tick_callback(handle)
            unreal.SystemLibrary.quit_editor()
    except Exception as e:  # leave, saying why
        log("FAILED", e)
        unreal.unregister_slate_post_tick_callback(handle)
        unreal.SystemLibrary.quit_editor()


handle = unreal.register_slate_post_tick_callback(tick)
