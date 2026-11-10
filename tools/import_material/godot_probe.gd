# Makes tests/fixtures/godot_visual in Godot 4 (headless): visual shaders and the ShaderMaterials using them, as
# Godot saves them, for kin_import_material's Tier 2 import (material_import3d.cpp). In a scratch project:
#   godot --headless --path <project> --script <this> textures   (writes res://textures/*.png)
#   godot --headless --path <project> --import                   (Godot imports them)
#   godot --headless --path <project> --script <this>            (the shaders and materials)
extends SceneTree

const F := VisualShader.TYPE_FRAGMENT

func png(path: String, color: Color) -> void:
	var image := Image.create(4, 4, false, Image.FORMAT_RGBA8)
	image.fill(color)
	image.save_png(path)

func node(vs: VisualShader, id: int, n: VisualShaderNode) -> VisualShaderNode:
	vs.add_node(F, n, Vector2(id * 40, 0), id)
	return n

func link(vs: VisualShader, a: int, ap: int, b: int, bp: int) -> void:
	var e := vs.connect_nodes(F, a, ap, b, bp)
	if e != OK:
		push_error("connect %d:%d -> %d:%d failed (%d)" % [a, ap, b, bp, e])

func constant(value: float) -> VisualShaderNodeFloatConstant:
	var c := VisualShaderNodeFloatConstant.new()
	c.constant = value
	return c

func input(name: String) -> VisualShaderNodeInput:
	var i := VisualShaderNodeInput.new()
	i.input_name = name
	return i

func _init() -> void:
	if OS.get_cmdline_user_args().has("textures") or "textures" in OS.get_cmdline_args():
		DirAccess.make_dir_recursive_absolute("res://textures")
		png("res://textures/albedo.png", Color(0.9, 0.6, 0.3, 0.5))
		png("res://textures/normal.png", Color(0.5, 0.5, 1.0))
		quit()
		return
	var albedo: Texture2D = load("res://textures/albedo.png")
	var normal: Texture2D = load("res://textures/normal.png")

	# VS_Mix: a tinted, tiled texture parameter; metallic from its red; a roughness parameter; a fresnel glow; a
	# panning normal map; ambient occlusion from the time's sine.
	var vs := VisualShader.new()
	vs.set_mode(Shader.MODE_SPATIAL)
	var tint := node(vs, 2, VisualShaderNodeColorParameter.new()) as VisualShaderNodeColorParameter
	tint.parameter_name = "tint"; tint.default_value_enabled = true; tint.default_value = Color(0.8, 0.5, 0.2, 1)
	(node(vs, 3, VisualShaderNodeTexture2DParameter.new()) as VisualShaderNodeTexture2DParameter).parameter_name = "albedo_tex"
	node(vs, 4, input("uv"))
	var tile := node(vs, 5, VisualShaderNodeVectorOp.new()) as VisualShaderNodeVectorOp
	tile.op_type = VisualShaderNodeVectorOp.OP_TYPE_VECTOR_2D; tile.operator = VisualShaderNodeVectorOp.OP_MUL
	(node(vs, 6, VisualShaderNodeVec2Constant.new()) as VisualShaderNodeVec2Constant).constant = Vector2(4, 4)
	var sample := node(vs, 7, VisualShaderNodeTexture.new()) as VisualShaderNodeTexture
	sample.source = VisualShaderNodeTexture.SOURCE_PORT
	var mul := node(vs, 8, VisualShaderNodeVectorOp.new()) as VisualShaderNodeVectorOp
	mul.op_type = VisualShaderNodeVectorOp.OP_TYPE_VECTOR_4D; mul.operator = VisualShaderNodeVectorOp.OP_MUL
	var rough := node(vs, 9, VisualShaderNodeFloatParameter.new()) as VisualShaderNodeFloatParameter
	rough.parameter_name = "rough"; rough.default_value_enabled = true; rough.default_value = 0.6
	node(vs, 10, constant(0.25))
	node(vs, 11, VisualShaderNodeFresnel.new())
	node(vs, 12, constant(3.0))
	var glow := node(vs, 13, VisualShaderNodeVectorOp.new()) as VisualShaderNodeVectorOp
	glow.operator = VisualShaderNodeVectorOp.OP_MUL
	(node(vs, 14, VisualShaderNodeVec3Constant.new()) as VisualShaderNodeVec3Constant).constant = Vector3(0, 0.5, 1)
	var bumps := node(vs, 15, VisualShaderNodeTexture.new()) as VisualShaderNodeTexture
	bumps.texture = normal; bumps.texture_type = VisualShaderNodeTexture.TYPE_NORMAL_MAP
	node(vs, 16, input("time"))
	(node(vs, 17, VisualShaderNodeFloatFunc.new()) as VisualShaderNodeFloatFunc).function = VisualShaderNodeFloatFunc.FUNC_SIN
	var clamp := node(vs, 18, VisualShaderNodeClamp.new()) as VisualShaderNodeClamp
	var split := node(vs, 19, VisualShaderNodeVectorDecompose.new()) as VisualShaderNodeVectorDecompose
	split.op_type = VisualShaderNodeVectorDecompose.OP_TYPE_VECTOR_4D
	(node(vs, 21, VisualShaderNodeFloatOp.new()) as VisualShaderNodeFloatOp).operator = VisualShaderNodeFloatOp.OP_MUL
	node(vs, 22, VisualShaderNodeUVFunc.new())
	(node(vs, 23, VisualShaderNodeVec2Constant.new()) as VisualShaderNodeVec2Constant).constant = Vector2(0.1, 0)
	link(vs, 4, 0, 5, 0); link(vs, 6, 0, 5, 1); link(vs, 5, 0, 7, 0); link(vs, 3, 0, 7, 2)
	link(vs, 7, 0, 8, 0); link(vs, 2, 0, 8, 1); link(vs, 8, 0, 0, 0)
	link(vs, 9, 0, 0, 3)
	link(vs, 7, 0, 19, 0); link(vs, 19, 0, 21, 0); link(vs, 10, 0, 21, 1); link(vs, 21, 0, 0, 2)
	link(vs, 12, 0, 11, 3); link(vs, 11, 0, 13, 0); link(vs, 14, 0, 13, 1); link(vs, 13, 0, 0, 5)
	link(vs, 23, 0, 22, 1); link(vs, 16, 0, 22, 2); link(vs, 22, 0, 15, 0); link(vs, 15, 0, 0, 9)
	link(vs, 16, 0, 17, 0); link(vs, 17, 0, 18, 0); link(vs, 18, 0, 0, 6)
	ResourceSaver.save(vs, "res://VS_Mix.tres")
	vs = load("res://VS_Mix.tres")

	var mat := ShaderMaterial.new()
	mat.shader = vs
	mat.set_shader_parameter("tint", Color(0.1, 0.2, 0.9, 1))
	mat.set_shader_parameter("albedo_tex", albedo)
	ResourceSaver.save(mat, "res://M_VisualMix.tres")
	var plain := ShaderMaterial.new()
	plain.shader = vs
	ResourceSaver.save(plain, "res://M_VisualDefault.tres")

	# VS_Cut: a cut-out by a mask parameter's alpha, unshaded, both sides; inline in its material.
	var cut := VisualShader.new()
	cut.set_mode(Shader.MODE_SPATIAL)
	cut.set("modes/cull", 2)
	cut.set("flags/unshaded", true)
	(node(cut, 2, VisualShaderNodeTexture2DParameter.new()) as VisualShaderNodeTexture2DParameter).parameter_name = "mask_tex"
	var m := node(cut, 3, VisualShaderNodeTexture.new()) as VisualShaderNodeTexture
	m.source = VisualShaderNodeTexture.SOURCE_PORT
	var a := node(cut, 4, VisualShaderNodeVectorDecompose.new()) as VisualShaderNodeVectorDecompose
	a.op_type = VisualShaderNodeVectorDecompose.OP_TYPE_VECTOR_4D
	node(cut, 5, constant(0.5))
	(node(cut, 6, VisualShaderNodeColorConstant.new()) as VisualShaderNodeColorConstant).constant = Color(0.2, 0.7, 0.3, 1)
	link(cut, 2, 0, 3, 2); link(cut, 3, 0, 4, 0); link(cut, 4, 3, 0, 1); link(cut, 5, 0, 0, 19); link(cut, 6, 0, 0, 0)
	var cutmat := ShaderMaterial.new()
	cutmat.shader = cut
	cutmat.set_shader_parameter("mask_tex", albedo)
	ResourceSaver.save(cutmat, "res://M_VisualCut.tres")

	# VS_Expr: an Expression node (kin has none) as the albedo.
	var ex := VisualShader.new()
	ex.set_mode(Shader.MODE_SPATIAL)
	node(ex, 2, input("time"))
	var e := node(ex, 3, VisualShaderNodeExpression.new()) as VisualShaderNodeExpression
	e.add_input_port(0, VisualShaderNode.PORT_TYPE_SCALAR, "t")
	e.add_output_port(0, VisualShaderNode.PORT_TYPE_VECTOR_3D, "c")
	e.expression = "c = vec3(sin(t));"
	link(ex, 2, 0, 3, 0); link(ex, 3, 0, 0, 0)
	ResourceSaver.save(ex, "res://VS_Expr.tres")
	var exmat := ShaderMaterial.new()
	exmat.shader = load("res://VS_Expr.tres")
	ResourceSaver.save(exmat, "res://M_VisualExpr.tres")
	print("probe written")
	quit()
