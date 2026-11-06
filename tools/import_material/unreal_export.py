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
import re
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
        self.functions = {}  # material functions' graphs, by path (each exported once)
        self.declarations = {}  # each asset's named reroute usages -> their declarations (from its T3D)

    def texture(self, tex):
        """The texture exported (once) as PNG (EXR for float ones); its path relative to the export, with its settings."""
        if tex is None:
            return None
        path = tex.get_path_name().split(".")[0]
        self.used.add(path)
        if path in self.textures:
            return self.textures[path]["file"]
        # PNG, else EXR (float sources), else HDR (a cube: unwrapped to longitude and latitude), each by the exporter
        # Unreal finds for the file type: it asks the exporter first, where forcing TextureExporterPNG asserts on a
        # float texture (Assertion failed: SupportsTexture), taking the editor down.
        ok, rel = False, None
        for ext in ((".hdr",) if isinstance(tex, unreal.TextureCube) else (".png", ".exr")):
            rel = "textures" + path + ext
            file = os.path.join(self.out, rel)
            os.makedirs(os.path.dirname(file), exist_ok=True)
            task = unreal.AssetExportTask()
            task.object = tex
            task.filename = file
            task.automated = True
            task.prompt = False
            task.replace_identical = True
            ok = unreal.Exporter.run_asset_export_task(task) and os.path.exists(file)
            if ok:
                break
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
        elif cls == "MaterialExpressionCollectionParameter":  # its default (a game may set it while it runs)
            c = prop("collection")
            for p in (c.get_editor_property("scalar_parameters") if c else []):
                if str(p.get_editor_property("parameter_name")) == str(name):
                    n["value"] = float(p.get_editor_property("default_value"))
            for p in (c.get_editor_property("vector_parameters") if c else []):
                if str(p.get_editor_property("parameter_name")) == str(name):
                    n["value"] = colour(p.get_editor_property("default_value"))
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

    def props(self, expression):
        """An expression's simple settings (numbers, flags, names, colours, vectors, enums), by their Python names."""
        out = {}
        skip = ("material_expression_editor_x", "material_expression_editor_y", "material_expression_guid", "desc",
                "outline", "comment", "show_output_name_on_pin", "collapsed")
        # (dir() lists the properties Blueprints see; the import's are tried by name too.)
        named = ("const_a", "const_b", "const_alpha", "const_exponent", "exponent", "base_reflect_fraction", "period",
                 "speed_x", "speed_y", "const_coordinate", "min_default", "max_default", "r", "g", "b", "a", "constant",
                 "default_value", "parameter_name", "luminance_factors", "scale", "levels", "output_min", "output_max",
                 "noise_function", "turbulence", "tiling", "repeat_size", "code", "output_type", "texture",
                 "sampler_type", "coordinate_index", "u_tiling", "v_tiling", "un_mirror_u", "un_mirror_v", "a_is_",
                 "default_value_bool", "x", "y", "z", "w", "fraction", "const_fraction", "power", "const_min",
                 "const_max", "const_value", "const_x", "const_y", "material_function", "declaration", "input_name",
                 "input_type", "preview_value", "use_preview_value_as_default", "sort_priority", "output_name",
                 "value", "bias", "clamp_result",
                 # Transform / TransformPosition, BumpOffset, DepthFade, LandscapeLayerCoords, CollectionParameter
                 "transform_source_type", "transform_type", "height_ratio", "reference_plane", "fade_distance_default",
                 "opacity_default", "mapping_type", "custom_uv_type", "mapping_scale", "mapping_rotation",
                 "mapping_pan_u", "mapping_pan_v", "collection",
                 # ViewProperty
                 "property")
        for key in sorted(set(dir(expression)) | set(named)):
            if key.startswith("_") or key in skip:
                continue
            try:
                v = expression.get_editor_property(key)
            except Exception:
                continue
            if isinstance(v, bool) or isinstance(v, (int, float)):
                out[key] = v
            elif isinstance(v, str) or isinstance(v, unreal.Name):
                out[key] = str(v)
            elif isinstance(v, unreal.LinearColor):
                out[key] = colour(v)
            elif isinstance(v, (unreal.Vector, unreal.Vector4)) or type(v).__name__ in ("Vector4", "Vector4d", "Vector4f"):
                c = [float(getattr(v, k)) for k in ("x", "y", "z", "w") if hasattr(v, k)]
                if not c:  # (Vector4f has its fields as editor properties only)
                    for k in ("x", "y", "z", "w"):
                        try:
                            c.append(float(v.get_editor_property(k)))
                        except Exception:
                            break
                out[key] = c
            elif isinstance(v, unreal.Vector2D):
                out[key] = [float(v.x), float(v.y)]
            elif isinstance(v, unreal.EnumBase):
                out[key] = enum_name(v)
            elif isinstance(v, unreal.Texture):
                out[key] = self.texture(v)
            elif isinstance(v, unreal.MaterialFunctionInterface):
                out[key] = self.function(v)  # its path; its graph in the document's "functions"
            elif isinstance(v, unreal.MaterialParameterCollection):
                # A collection parameter's default (the game may set it while it runs): "collection_value".
                out[key] = v.get_path_name()
                name = str(expression.get_editor_property("parameter_name"))
                for p in v.get_editor_property("scalar_parameters"):
                    if str(p.get_editor_property("parameter_name")) == name:
                        out["collection_value"] = float(p.get_editor_property("default_value"))
                for p in v.get_editor_property("vector_parameters"):
                    if str(p.get_editor_property("parameter_name")) == name:
                        out["collection_value"] = colour(p.get_editor_property("default_value"))
            elif isinstance(v, unreal.MaterialExpression):
                out[key] = v.get_name()  # an expression it refers to (a named reroute's declaration)
        return out

    def walker(self, nodes, inputs_of):
        """A visit(expression) adding each expression reached (by its name) to `nodes`: its class, settings and inputs
        (the expression and output feeding each, by the input's name); `inputs_of(expression)` lists its inputs."""

        def visit(expression):
            name = expression.get_name()
            if name in nodes:
                return name
            n = {"class": expression.get_class().get_name(), "props": self.props(expression), "inputs": {}}
            nodes[name] = n
            try:
                names = [str(x) for x in MEL.get_material_expression_input_names(expression)]
            except Exception:
                names = []
            try:
                inputs = inputs_of(expression)
            except Exception:
                inputs = []
            for i, source in enumerate(inputs):
                if source is None:
                    continue
                try:
                    out = MEL.get_input_node_output_name_for_material_expression(expression, source)
                except Exception:
                    out = ""
                key = names[i] if i < len(names) and names[i] else str(i)
                n["inputs"][key] = {"node": visit(source), "output": str(out)}
            return name

        return visit

    def function(self, func):
        """A material function's graph, exported once: its expressions as a material's, its inputs and outputs (each
        name -> its FunctionInput / FunctionOutput expression). A function instance's is its parent's."""
        path = func.get_path_name().split(".")[0]
        if path in self.functions:
            return path
        self.functions[path] = None  # (a function calling itself stops here)
        base = func
        instance = getattr(unreal, "MaterialFunctionInstance", None)
        while instance is not None and isinstance(base, instance):
            parent = base.get_editor_property("parent")
            if parent is None:
                break
            base = parent
        g = {"nodes": {}, "inputs": {}, "outputs": {}}
        try:
            expressions = MEL.get_material_function_expressions(base)
        except Exception as e:
            g["error"] = str(e)
            self.functions[path] = g
            return path
        visit = self.walker(g["nodes"], lambda e: MEL.get_inputs_for_material_function_expression(base, e))
        for e in expressions:
            cls = e.get_class().get_name()
            if cls == "MaterialExpressionFunctionInput":
                g["inputs"][str(e.get_editor_property("input_name"))] = visit(e)
            elif cls == "MaterialExpressionFunctionOutput":
                g["outputs"][str(e.get_editor_property("output_name"))] = visit(e)
        self.link_reroutes(base, g["nodes"], visit)
        self.functions[path] = g
        return path

    def t3d(self, asset):
        """What Python can't read of an asset's expressions, from its T3D text (ObjectExporterT3D), once an asset:
        each named reroute usage's declaration, and each expression's inputs in order (source, output index, mask)."""
        path = asset.get_path_name()
        if path in self.declarations:
            return self.declarations[path]
        found = {"declarations": {}, "inputs": {}, "function_outputs": {}}
        file = os.path.join(self.out, ".asset.t3d")
        task = unreal.AssetExportTask()
        task.object = asset
        task.filename = file
        task.automated = True
        task.prompt = False
        task.replace_identical = True
        task.exporter = unreal.ObjectExporterT3D()
        try:
            if unreal.Exporter.run_asset_export_task(task):
                current = None
                for line in open(file, encoding="utf-8", errors="replace"):
                    # (T3D lists each object twice: a Class= block, then its properties under Name= alone)
                    b = re.search(r'Begin Object (?:Class=\S+ )?Name="([^"]+)"', line)
                    if b:
                        current = b.group(1)
                        continue
                    if line.strip().startswith("End Object"):
                        current = None
                        continue
                    if not current:
                        continue
                    # A function call's outputs, in the order an input's OutputIndex counts them.
                    f = re.search(r'^\s*FunctionOutputs\((\d+)\)=.*?OutputName="([^"]*)"', line)
                    if f:
                        found["function_outputs"].setdefault(current, {})[int(f.group(1))] = f.group(2)
                    d = re.search(r"^\s*Declaration=\S*'[^']*:([A-Za-z0-9_]+)'", line)
                    if d:
                        found["declarations"][current] = d.group(1)
                    for e in re.finditer(r"Expression=\"[^\"]*'[^']*?([A-Za-z0-9_]+)'\"([^)]*)", line):
                        rest = e.group(2)
                        index = re.search(r"OutputIndex=(\d+)", rest)
                        mask = "".join(c for c in "RGBA" if ("Mask%s=1" % c) in rest) if "Mask=1" in rest else ""
                        found["inputs"].setdefault(current, []).append((e.group(1), int(index.group(1)) if index else 0,
                                                                        mask))
        except Exception as e:
            unreal.log_warning("unreal_export: no T3D of %s (%s)" % (path, e))
        finally:
            if os.path.exists(file):
                os.remove(file)
        self.declarations[path] = found
        return found

    def link_reroutes(self, asset, nodes, visit):
        """The T3D's part of a graph. Each named reroute usage's declaration (the usage's Declaration isn't readable from Python), from the T3D;
        a declaration reached only through its usages is visited too (and the usages it reaches, until none is left).
        Then the channels of inputs one source feeds more than once: Python's output name is the first input's for all
        (GetInputNodeOutputNameForMaterialExpression), the T3D has each one's mask."""
        if any(n["class"] == "MaterialExpressionNamedRerouteUsage" for n in nodes.values()):
            path = asset.get_path_name()
            links = self.t3d(asset)["declarations"]
            while True:
                todo = [k for k, n in nodes.items()
                        if n["class"] == "MaterialExpressionNamedRerouteUsage" and "declaration" not in n["props"]
                        and links.get(k)]
                if not todo:
                    break
                for u in todo:
                    d = links[u]
                    nodes[u]["props"]["declaration"] = d
                    if d not in nodes:
                        for where in (path + ":" + d, path + ":MaterialEditorOnlyData_0." + d):
                            e = unreal.find_object(None, where)
                            if e is not None:
                                visit(e)
                                break
        # Every input's channels by its mask (renamed channels, "UV scale_H", are R, G, B or A to the import), and
        # its output's index: the k-th input from a source is the k-th the T3D lists from it.
        found = self.t3d(asset)
        t3d = found["inputs"]
        # A function call's outputs by index (the pin's name the editor gives is not always the output's).
        for k, outputs in found.get("function_outputs", {}).items():
            if k in nodes and outputs:
                nodes[k]["props"]["function_outputs"] = [outputs.get(i, "") for i in range(max(outputs) + 1)]
        for k, n in nodes.items():
            listed = t3d.get(k, [])
            if not listed or not n["inputs"]:
                continue
            for source in {i["node"] for i in n["inputs"].values()}:
                ours = [i for i in n["inputs"].values() if i["node"] == source]
                theirs = [t for t in listed if t[0] == source]
                if len(theirs) != len(ours):
                    continue
                for i, t in zip(ours, theirs):
                    if t[2]:
                        i["output"] = t[2]
                    i["output_index"] = t[1]

    def functions_of(self, graph):
        """The functions a graph calls, and those they call, by path."""
        found, todo = {}, [graph]
        while todo:
            g = todo.pop()
            for n in (g or {}).get("nodes", {}).values():
                p = n["props"].get("material_function")
                if isinstance(p, str) and p not in found and self.functions.get(p) is not None:
                    found[p] = self.functions[p]
                    todo.append(found[p])
        return found

    def graph(self, base):
        """The base material's expressions as a graph: each by its name, its class, settings and inputs (each the
        expression and output feeding it, by name), and the outputs each feeds (kin_import_material's graph import)."""
        nodes = {}
        visit = self.walker(nodes, lambda e: MEL.get_inputs_for_material_expression(base, e))

        outputs = {}
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
            outputs[key] = {"node": visit(node), "output": str(out)}
        self.link_reroutes(base, nodes, visit)
        return {"nodes": nodes, "outputs": outputs}

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
        try:  # a surface's (MD_SURFACE), a post process's, a volume's, ...
            doc["material_domain"] = enum_name(base.get_editor_property("material_domain"))
        except Exception:
            pass
        try:  # its Normal in tangent space (the default), else in the world's
            doc["tangent_space_normal"] = bool(base.get_editor_property("tangent_space_normal"))
        except Exception:
            pass
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
        # Static switches (an instance's own values over its parents' and the defaults): they choose the graph.
        params["switch"] = {}
        try:
            names = MEL.get_static_switch_parameter_names(asset)
        except Exception:
            names = []
        for name in names or []:
            try:
                v = (MEL.get_material_instance_static_switch_parameter_value(asset, name)
                     if isinstance(asset, unreal.MaterialInstanceConstant)
                     else MEL.get_material_default_static_switch_parameter_value(base, name))
                params["switch"][str(name)] = bool(v)
            except Exception:
                pass
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
        doc["graph"] = self.graph(base)
        doc["functions"] = self.functions_of(doc["graph"])
        # The textures it names, its functions' too (a function exported for an earlier material marked them then).
        files = {n["props"].get("texture") for f in doc["functions"].values() for n in f["nodes"].values()}
        doc["textures"] = {i["file"]: {k: v for k, v in i.items() if k != "file"}
                           for p, i in self.textures.items() if i["file"] and (p in self.used or i["file"] in files)}
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
