# Writes tests/fixtures/unity (python3 tools/import_material/make_unity_fixture.py tests/fixtures/unity): a small Shader
# Graph (10+ format) and a sub-graph, as Unity saves them, for tests/material_import3d_tests.cpp.
import json, sys, os
root = sys.argv[1]
objs = []
def obj(t, oid, **kw):
    o = {"m_SGVersion": 0, "m_Type": "UnityEditor.ShaderGraph." + t, "m_ObjectId": oid}
    o.update(kw); objs.append(o); return o
def slot(oid, sid, name, kind, t, value=None, **kw):
    o = {"m_Id": sid, "m_DisplayName": name, "m_SlotType": kind}
    if value is not None: o["m_Value"] = value
    o.update(kw)
    return obj(t, oid, **o)
edges = []
def edge(a, sa, b, sb): edges.append({"m_OutputSlot": {"m_Node": {"m_Id": a}, "m_SlotId": sa}, "m_InputSlot": {"m_Node": {"m_Id": b}, "m_SlotId": sb}})
def node(t, oid, name, slots, **kw):
    return obj(t, oid, m_Name=name, m_Slots=[{"m_Id": s} for s in slots], **kw)
def tex(guid): return {"m_SerializedTexture": json.dumps({"texture": {"fileID": 2800000, "guid": guid, "type": 3}}), "m_Guid": ""}
def v4(x, y=0, z=0, w=0): return {"x": x, "y": y, "z": z, "w": w}

def write(path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", newline="\n") as f:
        f.write("\n\n".join(json.dumps(o, indent=4) for o in objs) + "\n\n")

# --- Invert.shadersubgraph: Out = 1 - In ---
graph = obj("GraphData", "g0", m_Properties=[{"m_Id": "p_in"}], m_Keywords=[], m_Nodes=[{"m_Id": n} for n in ("s_prop", "s_om", "s_out")], m_Edges=edges, m_OutputNode={"m_Id": "s_out"})
obj("Internal.Vector1ShaderProperty", "p_in", m_Guid={"m_GuidSerialized": "4f1c6b0e-0000-4000-8000-000000000001"}, m_Name="In", m_DefaultReferenceName="Vector1_In", m_OverrideReferenceName="", m_Value=0.0)
slot("ss_prop", 0, "In", 1, "Vector1MaterialSlot", 0.0)
node("PropertyNode", "s_prop", "Property", ["ss_prop"], m_Property={"m_Id": "p_in"})
slot("ss_om_in", 0, "In", 0, "DynamicVectorMaterialSlot", v4(0))
slot("ss_om_out", 1, "Out", 1, "DynamicVectorMaterialSlot", v4(0))
node("OneMinusNode", "s_om", "One Minus", ["ss_om_in", "ss_om_out"])
slot("ss_out", 1, "Out", 0, "Vector1MaterialSlot", 0.0)
node("SubGraphOutputNode", "s_out", "Output", ["ss_out"])
edge("s_prop", 0, "s_om", 0); edge("s_om", 1, "s_out", 1)
write(f"{root}/Assets/Shaders/Invert.shadersubgraph")

# --- Lit.shadergraph ---
objs.clear(); edges.clear()
nodes = []
def n(t, oid, name, slots, **kw):
    nodes.append(oid); return node(t, oid, name, slots, **kw)
obj("GraphData", "g1", m_Properties=[{"m_Id": p} for p in ("p_color", "p_smooth", "p_base", "p_mask", "p_normal")],
    m_Keywords=[{"m_Id": "k_glow"}], m_Nodes=[{"m_Id": x} for x in nodes], m_Edges=edges, m_OutputNode={"m_Id": ""},
    m_ActiveTargets=[{"m_Id": "t_urp"}])
obj("Internal.ColorShaderProperty", "p_color", m_Guid={"m_GuidSerialized": "c-1"}, m_Name="Base Color", m_DefaultReferenceName="Color_1", m_OverrideReferenceName="_BaseColor", m_Value={"r": 0.5, "g": 0.5, "b": 0.5, "a": 1.0})
obj("Internal.Vector1ShaderProperty", "p_smooth", m_Guid={"m_GuidSerialized": "c-2"}, m_Name="Smoothness", m_DefaultReferenceName="_Smoothness", m_OverrideReferenceName="", m_Value=0.3)
obj("Internal.Texture2DShaderProperty", "p_base", m_Guid={"m_GuidSerialized": "c-3"}, m_Name="Base Map", m_DefaultReferenceName="_BaseMap", m_OverrideReferenceName="", m_Value=tex("a0000000000000000000000000000001"))
obj("Internal.Texture2DShaderProperty", "p_mask", m_Guid={"m_GuidSerialized": "c-4"}, m_Name="Mask Map", m_DefaultReferenceName="Texture2D_8E1D2A", m_OverrideReferenceName="", m_Value=tex("a0000000000000000000000000000005"))
obj("Internal.Texture2DShaderProperty", "p_normal", m_Guid={"m_GuidSerialized": "c-5"}, m_Name="Normal Map", m_DefaultReferenceName="_NormalMap", m_OverrideReferenceName="", m_Value=tex("a0000000000000000000000000000004"))
obj("ShaderKeyword", "k_glow", m_Name="Glow", m_DefaultReferenceName="BOOLEAN_GLOW", m_OverrideReferenceName="_GLOW", m_KeywordType=0, m_Value=0)
obj("Rendering.Universal.ShaderGraph.UniversalTarget", "t_urp", m_ActiveSubTarget={"m_Id": "t_lit"}, m_SurfaceType=0, m_AlphaMode=0, m_TwoSided=True, m_AlphaClip=True)
obj("Rendering.Universal.ShaderGraph.UniversalLitSubTarget", "t_lit", m_WorkflowMode=1)
for pid, prop, t in (("n_color", "p_color", "Vector4MaterialSlot"), ("n_smooth", "p_smooth", "Vector1MaterialSlot"),
                     ("n_base", "p_base", "Texture2DMaterialSlot"), ("n_mask", "p_mask", "Texture2DMaterialSlot"),
                     ("n_normal", "p_normal", "Texture2DMaterialSlot")):
    slot(pid + "_o", 0, "Out", 1, t)
    n("PropertyNode", pid, "Property", [pid + "_o"], m_Property={"m_Id": prop})
slot("uv_o", 0, "Out", 1, "Vector4MaterialSlot", v4(0))
n("UVNode", "n_uv", "UV", ["uv_o"], m_OutputChannel=0)
slot("to_uv", 0, "UV", 0, "UVMaterialSlot", {"x": 0, "y": 0}); slot("to_t", 1, "Tiling", 0, "Vector2MaterialSlot", {"x": 2, "y": 3})
slot("to_o", 2, "Offset", 0, "Vector2MaterialSlot", {"x": 0.5, "y": 0}); slot("to_out", 3, "Out", 1, "Vector2MaterialSlot", {"x": 0, "y": 0})
n("TilingAndOffsetNode", "n_to", "Tiling And Offset", ["to_uv", "to_t", "to_o", "to_out"])
def sampler(oid, name, texture_type):
    ids = []
    for sid, nm, kind, t in ((0, "RGBA", 1, "Vector4MaterialSlot"), (4, "R", 1, "Vector1MaterialSlot"), (5, "G", 1, "Vector1MaterialSlot"),
                             (6, "B", 1, "Vector1MaterialSlot"), (7, "A", 1, "Vector1MaterialSlot"), (1, "Texture", 0, "Texture2DInputMaterialSlot"),
                             (2, "UV", 0, "UVMaterialSlot"), (3, "Sampler", 0, "SamplerStateMaterialSlot")):
        slot(f"{oid}_{sid}", sid, nm, kind, t, **({"m_Texture": tex("")} if sid == 1 else {}))
        ids.append(f"{oid}_{sid}")
    n("SampleTexture2DNode", oid, name, ids, m_TextureType=texture_type, m_NormalMapSpace=0)
sampler("n_tb", "Sample Base", 0); sampler("n_tm", "Sample Mask", 0); sampler("n_tn", "Sample Normal", 1)
def binary(t, oid, name, b=None):
    slot(oid + "_a", 0, "A", 0, "DynamicValueMaterialSlot" if t == "MultiplyNode" else "DynamicVectorMaterialSlot", v4(0))
    slot(oid + "_b", 1, "B", 0, "DynamicValueMaterialSlot" if t == "MultiplyNode" else "DynamicVectorMaterialSlot", b or v4(2, 2, 2, 2))
    slot(oid + "_out", 2, "Out", 1, "DynamicValueMaterialSlot" if t == "MultiplyNode" else "DynamicVectorMaterialSlot", v4(0))
    n(t, oid, name, [oid + "_a", oid + "_b", oid + "_out"])
binary("MultiplyNode", "n_mul", "Multiply")
binary("AddNode", "n_add", "Add", v4(0.25))
slot("sub_in", 101, "In", 0, "Vector1MaterialSlot", 0.0); slot("sub_out", 1, "Out", 1, "Vector1MaterialSlot", 0.0)
n("SubGraphNode", "n_sub", "Invert", ["sub_in", "sub_out"],
  m_SerializedSubGraph=json.dumps({"subGraph": {"fileID": -5475051401550479605, "guid": "c0000000000000000000000000000001", "type": 3}}),
  m_PropertyGuids=["4f1c6b0e-0000-4000-8000-000000000001"], m_PropertyIds=[101])
for sid, nm in ((0, "Time"), (1, "Sine Time"), (2, "Cosine Time"), (3, "Delta Time"), (4, "Smooth Delta")):
    slot(f"time_{sid}", sid, nm, 1, "Vector1MaterialSlot", 0.0)
n("TimeNode", "n_time", "Time", [f"time_{s}" for s in range(5)])
slot("kw_out", 0, "Out", 1, "DynamicVectorMaterialSlot", v4(0)); slot("kw_on", 1, "On", 0, "DynamicVectorMaterialSlot", v4(0))
slot("kw_off", 2, "Off", 0, "DynamicVectorMaterialSlot", v4(0))
n("KeywordNode", "n_kw", "Glow", ["kw_out", "kw_on", "kw_off"], m_Keyword={"m_Id": "k_glow"})
slot("cu_in", 0, "In", 0, "Vector1MaterialSlot", 0.0); slot("cu_out", 1, "Out", 1, "Vector1MaterialSlot", 0.0)
n("CustomFunctionNode", "n_custom", "Wobble (Custom Function)", ["cu_in", "cu_out"])
def block(oid, descriptor, t, value):
    slot(oid + "_s", 0, descriptor.split(".")[1], 0, t, value)
    n("BlockNode", oid, descriptor, [oid + "_s"], m_SerializedDescriptor=descriptor)
block("b_base", "SurfaceDescription.BaseColor", "ColorRGBMaterialSlot", {"x": 0.5, "y": 0.5, "z": 0.5})
block("b_smooth", "SurfaceDescription.Smoothness", "Vector1MaterialSlot", 0.5)
block("b_metal", "SurfaceDescription.Metallic", "Vector1MaterialSlot", 0.0)
block("b_normal", "SurfaceDescription.NormalTS", "NormalMaterialSlot", {"x": 0, "y": 0, "z": 1})
block("b_emit", "SurfaceDescription.Emission", "ColorRGBMaterialSlot", {"x": 0, "y": 0, "z": 0})
block("b_occ", "SurfaceDescription.Occlusion", "Vector1MaterialSlot", 1.0)
block("b_alpha", "SurfaceDescription.Alpha", "Vector1MaterialSlot", 1.0)
block("b_clip", "SurfaceDescription.AlphaClipThreshold", "Vector1MaterialSlot", 0.4)
block("b_pos", "VertexDescription.Position", "PositionMaterialSlot", {"x": 0, "y": 0, "z": 0})
edge("n_uv", 0, "n_to", 0); edge("n_base", 0, "n_tb", 1); edge("n_to", 3, "n_tb", 2)
edge("n_tb", 0, "n_mul", 0); edge("n_color", 0, "n_mul", 1); edge("n_mul", 2, "b_base", 0)
edge("n_smooth", 0, "n_add", 0); edge("n_add", 2, "b_smooth", 0)
edge("n_mask", 0, "n_tm", 1); edge("n_tm", 4, "n_sub", 101); edge("n_sub", 1, "b_metal", 0)
edge("n_normal", 0, "n_tn", 1); edge("n_tn", 0, "b_normal", 0)
edge("n_time", 1, "n_kw", 1); edge("n_kw", 0, "b_emit", 0)
edge("n_custom", 1, "b_occ", 0); edge("n_time", 0, "b_pos", 0)
objs[0]["m_Nodes"] = [{"m_Id": x} for x in nodes]
write(f"{root}/Assets/Shaders/Lit.shadergraph")
for f, guid in (("Invert.shadersubgraph", "c0000000000000000000000000000001"), ("Lit.shadergraph", "c0000000000000000000000000000002")):
    with open(f"{root}/Assets/Shaders/{f}.meta", "w", newline="\n") as m:
        m.write(f"fileFormatVersion: 2\nguid: {guid}\nScriptedImporter:\n")
