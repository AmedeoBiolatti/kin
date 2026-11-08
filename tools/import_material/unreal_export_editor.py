"""unreal_export.py in the editor (rendering: a cube texture's export, an unwrap to longitude and latitude, draws it;
the commandlet's has no render targets of it and asserts). Started by the project's Content/Python/init_unreal.py:

    LINEUP_SCRIPT=<this file> EXPORT_CONTENT=/Game/<folder> EXPORT_OUT=<folder> UnrealEditor <Project.uproject>

The cube textures are loaded first, texture streaming off, and the export waits (EXPORT_WAIT seconds, 30 by default)
for them to build and stream in: a cube exported before is the engine's placeholder (grey gradients a face). Then the
export runs and the editor quits.
"""
import os
import sys
import time

import unreal

HERE = os.path.dirname(os.path.abspath(os.environ.get("LINEUP_SCRIPT", __file__)))
state = {"started": None, "cubes": []}


def preload():
    unreal.SystemLibrary.execute_console_command(None, "r.TextureStreaming 0")
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    for a in registry.get_assets_by_path(os.environ["EXPORT_CONTENT"], recursive=True):
        if str(a.asset_class_path.asset_name) == "TextureCube":
            state["cubes"].append(unreal.load_asset(str(a.package_name)))  # (kept: loaded until the export)
    unreal.log("unreal_export_editor: %d cube textures loaded, waiting" % len(state["cubes"]))


def tick(_dt):
    if state["started"] is None:
        state["started"] = time.time()
        preload()
        return
    if time.time() - state["started"] < float(os.environ.get("EXPORT_WAIT", "30")):
        return
    unreal.unregister_slate_post_tick_callback(handle)
    try:
        sys.argv = ["unreal_export.py", "--content", os.environ["EXPORT_CONTENT"], "--out", os.environ["EXPORT_OUT"]]
        script = os.path.join(HERE, "unreal_export.py")
        exec(compile(open(script).read(), script, "exec"), {"__name__": "__main__"})
        unreal.log("unreal_export_editor: done")
    except Exception as e:
        unreal.log_error("unreal_export_editor: FAILED " + str(e))
    unreal.SystemLibrary.quit_editor()


handle = unreal.register_slate_post_tick_callback(tick)
