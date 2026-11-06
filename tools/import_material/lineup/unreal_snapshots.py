"""Snapshots of a project's own materials in Unreal 5 (snapshot.json: one material on a quad and a sphere, lit as the
lineup is), to compare with kin_material_lineup --material's pictures of their imports. Run in the editor of the
project holding the materials, with the lineup's render settings (AA, GI, reflections, AO, auto exposure, virtual
shadow maps and texture streaming off: see docs/rendering_3d.md) and its Content/Python/init_unreal.py:

    LINEUP_SCRIPT=<this file> SNAP_SCENE=<snapshot.json> SNAP_ASSETS=<folder of quad.glb, sphere.glb> \\
    SNAP_LIST=<file: a material a line> SNAP_OUT=<folder> UnrealEditor <Project.uproject>

A line of SNAP_LIST is a material's path as kin_import_material wrote it, "m/<pack>/.../<name>" (its .kinmaterials
without the extension: /Game/<pack>/.../<name> here), then anything (a note, for snapshot_report.py); # a comment.
Each picture is SNAP_OUT/<that path, / as __>.png; a material that cannot be loaded is said in the log (lineup:
missing) and skipped. Settings as unreal_lineup.py's.

Unlike the lineup, the scene is placed by Unreal's own glTF axes (kin's x, y, z metres are Unreal's X, Z, Y x 100 cm:
its exporter's mapping, Interchange's, and kin_import_material's for World Position and the other world vectors), so
that a material patterned on where a pixel is in the world sees the same place in both engines.
"""
import json
import math
import os

import unreal

ROOT = "/Game/Lineup"
DIR = os.environ["SNAP_ASSETS"]
OUT = os.environ["SNAP_OUT"]
SCENE = json.load(open(os.environ["SNAP_SCENE"]))
MATERIALS = [line.split()[0] for line in open(os.environ["SNAP_LIST"]) if line.strip() and not line.startswith("#")]
TOOLS = unreal.AssetToolsHelpers.get_asset_tools()


def log(*a):
    unreal.log("lineup: " + " ".join(str(x) for x in a))


def ue(p, scale=100.0):
    return unreal.Vector(p[0] * scale, p[2] * scale, p[1] * scale)


def unreal_path(kin_path):
    rest = kin_path[2:] if kin_path.startswith("m/") else kin_path
    name = rest.rsplit("/", 1)[-1]
    return "/Game/" + rest + "." + name


def picture_name(kin_path):
    return kin_path.replace("/", "__") + ".png"


def mesh(name):
    task = unreal.AssetImportTask()
    task.filename = os.path.join(DIR, name + ".glb")
    task.destination_path = ROOT + "/Meshes/" + name
    task.automated = True
    task.save = True
    task.replace_existing = True
    TOOLS.import_asset_tasks([task])
    for p in list(task.get_editor_property("imported_object_paths")) + list(
            unreal.EditorAssetLibrary.list_assets(ROOT + "/Meshes/" + name, recursive=True)):
        a = unreal.load_asset(p)
        if isinstance(a, unreal.StaticMesh):
            return a
    raise RuntimeError("no mesh in " + name + ".glb")


def build():
    meshes = {"quad": mesh("quad"), "sphere": mesh("sphere")}
    level = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    # A fresh level of a name of its own each run (new_level fails on an existing map, and the editor reopens the last
    # run's, which cannot be deleted while open); the runs' others deleted where they can be.
    import time
    name = ROOT + "/Snapshot_" + time.strftime("%Y%m%d_%H%M%S")
    for old in unreal.EditorAssetLibrary.list_assets(ROOT, recursive=False):
        if "/Snapshot" in old and isinstance(unreal.load_asset(old), unreal.World):
            try:
                unreal.EditorAssetLibrary.delete_asset(old)
            except Exception:
                pass
    if not level.new_level(name):
        raise RuntimeError("cannot make the level " + name)
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    components = []
    for o in SCENE["objects"]:
        # (Interchange brings a glTF in by the same axes: its front, +z, is Unreal's +Y, toward the camera)
        a = actors.spawn_actor_from_class(unreal.StaticMeshActor, ue(o["position"]), unreal.Rotator(0, 0, 0))
        c = a.static_mesh_component
        c.set_mobility(unreal.ComponentMobility.MOVABLE)
        c.set_static_mesh(meshes[o["mesh"]])
        c.set_cast_shadow(False)
        components.append(c)
    t = SCENE["sun"]["toward"]
    sun = actors.spawn_actor_from_class(unreal.DirectionalLight, unreal.Vector(0, 0, 500),
                                        unreal.MathLibrary.make_rot_from_x(ue([-v for v in t], 1.0)))  # from the light
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
    look = ue([t - e for t, e in zip(cam["target"], cam["eye"])], 1.0)
    camera = actors.spawn_actor_from_class(unreal.CameraActor, ue(cam["eye"]),
                                           unreal.MathLibrary.make_rot_from_x(look))
    cc = camera.camera_component
    cc.set_editor_property("field_of_view",
                           math.degrees(2 * math.atan(math.tan(math.radians(cam["fov_y"]) / 2) * w / h)))
    cc.set_editor_property("constrain_aspect_ratio", True)
    cc.set_editor_property("aspect_ratio", w / h)
    level.save_current_level()
    return camera, components


# A material at a time: set (wait for its shaders and textures), pictured, then the next.
state = {"tick": 0, "camera": None, "components": [], "index": -1, "phase": None, "at": 0, "task": None}


def next_material():
    while True:
        state["index"] += 1
        if state["index"] >= len(MATERIALS):
            return False
        path = MATERIALS[state["index"]]
        m = unreal.load_asset(unreal_path(path))
        if isinstance(m, unreal.MaterialInterface):
            for c in state["components"]:
                c.set_material(0, m)
            log("set", path)
            return True
        log("missing", path, unreal_path(path))


def tick(_dt):
    state["tick"] += 1
    n = state["tick"]
    try:
        if n == 60:
            state["camera"], state["components"] = build()
            os.makedirs(OUT, exist_ok=True)
            log("built", len(MATERIALS), "materials")
            state["phase"], state["at"] = "next", n
        elif state["phase"] == "next" and n > state["at"] + 5:
            if not next_material():
                log("done")
                unreal.unregister_slate_post_tick_callback(handle)
                unreal.SystemLibrary.quit_editor()
                return
            state["phase"], state["at"] = "load", n
        elif state["phase"] == "load" and n > state["at"] + 30:
            unreal.AutomationLibrary.finish_loading_before_screenshot()  # shaders compiled, textures resident
            state["phase"], state["at"] = "settle", n
        elif state["phase"] == "settle" and n > state["at"] + 60:
            w, h = SCENE["size"]
            target = os.path.join(OUT, picture_name(MATERIALS[state["index"]]))
            state["task"] = unreal.AutomationLibrary.take_high_res_screenshot(w, h, target, state["camera"])
            # The world's clock as the picture is taken (a material's Time: kin_material_lineup --time, to match)
            world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
            log("time", MATERIALS[state["index"]], unreal.GameplayStatics.get_time_seconds(world),
                unreal.GameplayStatics.get_real_time_seconds(world))
            state["phase"], state["at"] = "shot", n
        elif state["phase"] == "shot" and state["task"].is_task_done():
            log("pictured", MATERIALS[state["index"]])
            state["phase"], state["at"] = "next", n
    except Exception as e:  # leave, saying why
        log("FAILED", e)
        unreal.unregister_slate_post_tick_callback(handle)
        unreal.SystemLibrary.quit_editor()


handle = unreal.register_slate_post_tick_callback(tick)
