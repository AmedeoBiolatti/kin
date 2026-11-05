"""Unreal's materials as kin.unreal-material/1 files, for kin_import_material unreal (kin/render3d/material_import3d.hpp).

Run inside Unreal's editor (5.x, the Python Editor Script Plugin), headless:

    UnrealEditor-Cmd <Project.uproject> -run=pythonscript -EnablePlugins=PythonScriptPlugin \
        -script="<this file> --content /Game/Path --out /abs/export/folder" -unattended -nosplash -nullrhi

Writes, for each Material and MaterialInstance under --content: <out>/<its path under /Game>.json with its blend mode,
shading model, two-sidedness, clip value, its parameters' values (an instance's overrides over its parents'), and
what feeds each of the material's outputs (Base Color, Metallic, Roughness, Normal, Emissive Color, Opacity, Opacity
Mask, Ambient Occlusion, ...), a few expressions deep; and each texture they name as <out>/textures/<its path>.png
with its sRGB, compression and green-flip settings. Nothing in the project is changed or saved.
"""
import json
import os
import sys

import unreal

DEPTH = 6
PROPERTIES = {
    "BaseColor": "MP_BASE_COLOR", "Metallic": "MP_METALLIC", "Specular": "MP_SPECULAR",
    "Roughness": "MP_ROUGHNESS", "EmissiveColor": "MP_EMISSIVE_COLOR", "Opacity": "MP_OPACITY",
    "OpacityMask": "MP_OPACITY_MASK", "Normal": "MP_NORMAL", "AmbientOcclusion": "MP_AMBIENT_OCCLUSION",
    "WorldPositionOffset": "MP_WORLD_POSITION_OFFSET", "Anisotropy": "MP_ANISOTROPY", "Refraction": "MP_REFRACTION",
}
MEL = unreal.MaterialEditingLibrary


def args():
    out = {"content": "/Game", "out": None}
    argv = sys.argv[1:]
    for i, a in enumerate(argv):
        if a in ("--content", "--out") and i + 1 < len(argv):
            out[a[2:]] = argv[i + 1]
    if not out["out"]:
        raise SystemExit("unreal_export.py: --out is required")
    return out


def enum_name(value):
    text = str(value)
    return text.split(".")[-1].split(":")[0].strip("<> ").upper()


def colour(c):
    return [float(c.r), float(c.g), float(c.b), float(c.a)]


class Exporter:
    def __init__(self, out):
        self.out = out
        self.textures = {}
        self.used = set()  # the textures the material being exported names

    def texture(self, tex):
        """The texture exported (once) as PNG; its path relative to the export, with its settings."""
        if tex is None:
            return None
        path = tex.get_path_name().split(".")[0]
        self.used.add(path)
        if path in self.textures:
            return self.textures[path]["file"]
        rel = "textures" + path + ".png"
        file = os.path.join(self.out, rel)
        os.makedirs(os.path.dirname(file), exist_ok=True)
        task = unreal.AssetExportTask()
        task.object = tex
        task.filename = file
        task.automated = True
        task.prompt = False
        task.replace_identical = True
        task.exporter = unreal.TextureExporterPNG()
        ok = unreal.Exporter.run_asset_export_task(task) and os.path.exists(file)
        info = {"file": rel if ok else None, "asset": path}
        for key in ("srgb", "flip_green_channel", "compression_settings"):
            try:
                v = tex.get_editor_property(key)
                info[key] = v if isinstance(v, bool) else enum_name(v)
            except Exception:
                pass
        self.textures[path] = info
        return info["file"]

    def node(self, material, expression, output, depth):
        """What an expression is, as far as the import reads: its class, settings, and its inputs' nodes."""
        if expression is None:
            return None
        cls = expression.get_class().get_name()
        n = {"class": cls, "output": output or ""}
        get = expression.get_editor_property

        def prop(key):
            try:
                return get(key)
            except Exception:
                return None

        name = prop("parameter_name")
        if name is not None:
            n["parameter"] = str(name)
        if cls in ("MaterialExpressionTextureSample", "MaterialExpressionTextureSampleParameter2D",
                   "MaterialExpressionTextureObjectParameter", "MaterialExpressionTextureObject"):
            n["texture"] = self.texture(prop("texture"))
            st = prop("sampler_type")
            if st is not None:
                n["sampler_type"] = enum_name(st)
        elif cls == "MaterialExpressionConstant":
            n["value"] = float(prop("r"))
        elif cls == "MaterialExpressionConstant2Vector":
            n["value"] = [float(prop("r")), float(prop("g"))]
        elif cls in ("MaterialExpressionConstant3Vector", "MaterialExpressionConstant4Vector"):
            n["value"] = colour(prop("constant"))
        elif cls == "MaterialExpressionScalarParameter":
            n["value"] = float(prop("default_value"))
        elif cls == "MaterialExpressionVectorParameter":
            n["value"] = colour(prop("default_value"))
        elif cls in ("MaterialExpressionMultiply", "MaterialExpressionAdd", "MaterialExpressionSubtract",
                     "MaterialExpressionLinearInterpolate"):
            for k in ("const_a", "const_b", "const_alpha"):
                v = prop(k)
                if v is not None:
                    n[k] = float(v)
        elif cls == "MaterialExpressionComponentMask":
            n["mask"] = [bool(prop(c)) for c in ("r", "g", "b", "a")]
        elif cls == "MaterialExpressionTextureCoordinate":
            n["index"] = int(prop("coordinate_index") or 0)
            n["u_tiling"] = float(prop("u_tiling") or 1.0)
            n["v_tiling"] = float(prop("v_tiling") or 1.0)
        if depth > 0:
            inputs = []
            try:
                for i in MEL.get_inputs_for_material_expression(material, expression):
                    if i is None:
                        inputs.append(None)
                        continue
                    try:
                        out = MEL.get_input_node_output_name_for_material_expression(expression, i)
                    except Exception:
                        out = ""
                    inputs.append(self.node(material, i, out, depth - 1))
            except Exception as e:  # an older API: the node alone
                n["inputs_error"] = str(e)
            if inputs:
                n["inputs"] = inputs
        return n

    def material(self, asset):
        """One Material or MaterialInstance as a kin.unreal-material/1 document."""
        self.used = set()
        base = asset
        parents = []
        while isinstance(base, unreal.MaterialInstance):
            parent = base.get_editor_property("parent")
            if parent is None:
                break
            parents.append(parent.get_path_name().split(".")[0])
            base = parent
        if not isinstance(base, unreal.Material):
            return None
        doc = {"schema": "kin.unreal-material/1", "name": asset.get_name(),
               "path": asset.get_path_name().split(".")[0], "class": asset.get_class().get_name(),
               "parents": parents, "base": base.get_path_name().split(".")[0]}
        blend, two_sided, clip = base.get_editor_property("blend_mode"), base.get_editor_property("two_sided"), \
            base.get_editor_property("opacity_mask_clip_value")
        model = base.get_editor_property("shading_model")
        if isinstance(asset, unreal.MaterialInstance):
            o = asset.get_editor_property("base_property_overrides")
            if o.get_editor_property("override_blend_mode"):
                blend = o.get_editor_property("blend_mode")
            if o.get_editor_property("override_two_sided"):
                two_sided = o.get_editor_property("two_sided")
            if o.get_editor_property("override_opacity_mask_clip_value"):
                clip = o.get_editor_property("opacity_mask_clip_value")
            try:
                if o.get_editor_property("override_shading_model"):
                    model = o.get_editor_property("shading_model")
            except Exception:
                pass
        doc.update({"blend_mode": enum_name(blend), "shading_model": enum_name(model), "two_sided": bool(two_sided),
                    "opacity_mask_clip_value": float(clip)})
        # The parameters' values as this asset has them (an instance's own, else its parents', else the defaults).
        params = {"scalar": {}, "vector": {}, "texture": {}}
        instance = isinstance(asset, unreal.MaterialInstance)
        for name in MEL.get_scalar_parameter_names(asset):
            params["scalar"][str(name)] = float(MEL.get_material_instance_scalar_parameter_value(asset, name)
                                                if instance else MEL.get_material_default_scalar_parameter_value(asset, name))
        for name in MEL.get_vector_parameter_names(asset):
            params["vector"][str(name)] = colour(MEL.get_material_instance_vector_parameter_value(asset, name)
                                                 if instance else MEL.get_material_default_vector_parameter_value(asset, name))
        for name in MEL.get_texture_parameter_names(asset):
            tex = (MEL.get_material_instance_texture_parameter_value(asset, name) if instance
                   else MEL.get_material_default_texture_parameter_value(asset, name))
            params["texture"][str(name)] = self.texture(tex)
        doc["parameters"] = params
        inputs = {}
        for key, prop in PROPERTIES.items():
            p = getattr(unreal.MaterialProperty, prop, None)
            if p is None:
                continue
            try:
                node = MEL.get_material_property_input_node(base, p)
            except Exception:
                node = None
            if node is None:
                continue
            try:
                out = MEL.get_material_property_input_node_output_name(base, p)
            except Exception:
                out = ""
            inputs[key] = self.node(base, node, out, DEPTH)
        doc["inputs"] = inputs
        doc["textures"] = {i["file"]: {k: v for k, v in i.items() if k != "file"}
                           for p, i in self.textures.items() if i["file"] and p in self.used}
        return doc


def main():
    a = args()
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    exporter = Exporter(a["out"])
    count = 0
    for data in registry.get_assets_by_path(a["content"], recursive=True):
        cls = str(data.asset_class_path.asset_name) if hasattr(data, "asset_class_path") else str(data.asset_class)
        if cls not in ("Material", "MaterialInstanceConstant"):
            continue
        asset = unreal.load_asset(str(data.package_name))
        doc = exporter.material(asset) if asset else None
        if not doc:
            continue
        rel = doc["path"][len("/Game"):] if doc["path"].startswith("/Game") else doc["path"]
        file = os.path.join(a["out"], rel.lstrip("/") + ".json")
        os.makedirs(os.path.dirname(file), exist_ok=True)
        with open(file, "w") as f:
            json.dump(doc, f, indent=1)
        count += 1
    unreal.log("unreal_export: %d materials, %d textures to %s" % (count, len(exporter.textures), a["out"]))


main()
