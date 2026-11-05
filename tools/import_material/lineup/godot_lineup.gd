# The material lineup (lineup.json) in Godot 4: its materials as VisualShaders (saved, for kin_import_material godot
# --graphs) and its picture. In a Godot project holding the assets (make_assets.py) and this file:
#   godot --headless --path <project> --import
#   godot --path <project> --script res://godot_lineup.gd -- --out <picture.png>
# A 1600 x 800 SubViewport: Forward+, linear tone mapping at exposure 1, no AA, one sun (no shadow), no ambient.
extends SceneTree

const F := VisualShader.TYPE_FRAGMENT
var vs: VisualShader
var next_id := 2

func add(n: VisualShaderNode) -> int:
	var id := next_id
	next_id += 1
	vs.add_node(F, n, Vector2(id * 30, 0), id)
	return id

func link(a: int, ap: int, b: int, bp: int) -> void:
	if vs.connect_nodes(F, a, ap, b, bp) != OK:
		push_error("connect %d:%d -> %d:%d" % [a, ap, b, bp])

func fconst(v: float) -> int:
	var c := VisualShaderNodeFloatConstant.new(); c.constant = v; return add(c)

func v3const(v: Vector3) -> int:
	var c := VisualShaderNodeVec3Constant.new(); c.constant = v; return add(c)

func v2const(v: Vector2) -> int:
	var c := VisualShaderNodeVec2Constant.new(); c.constant = v; return add(c)

func input(name: String) -> int:
	var i := VisualShaderNodeInput.new(); i.input_name = name; return add(i)

func fop(op: int, a: int, b: int) -> int:
	var n := VisualShaderNodeFloatOp.new(); n.operator = op
	var id := add(n); link(a, 0, id, 0); link(b, 0, id, 1); return id

func ffunc(f: int, x: int) -> int:
	var n := VisualShaderNodeFloatFunc.new(); n.function = f
	var id := add(n); link(x, 0, id, 0); return id

func split(x: int, op_type: int) -> int:
	var n := VisualShaderNodeVectorDecompose.new(); n.op_type = op_type
	var id := add(n); link(x, 0, id, 0); return id

func compose(x: int, xp: int, y: int, yp: int, z: int, zp: int) -> int:
	var id := add(VisualShaderNodeVectorCompose.new())
	link(x, xp, id, 0); link(y, yp, id, 1); link(z, zp, id, 2); return id

func uv_times(k: float) -> int:
	var n := VisualShaderNodeVectorOp.new(); n.op_type = VisualShaderNodeVectorOp.OP_TYPE_VECTOR_2D
	n.operator = VisualShaderNodeVectorOp.OP_MUL
	var id := add(n); link(input("uv"), 0, id, 0); link(v2const(Vector2(k, k)), 0, id, 1); return id

func begin(unshaded: bool) -> void:
	vs = VisualShader.new()
	vs.set_mode(Shader.MODE_SPATIAL)
	if unshaded:
		vs.set("flags/unshaded", true)
	next_id = 2

func material(name: String, params := {}) -> ShaderMaterial:
	var m := ShaderMaterial.new()
	m.shader = vs
	m.resource_name = name
	for k in params:
		m.set_shader_parameter(k, params[k])
	if ResourceSaver.save(m, "res://materials/%s.tres" % name) != OK:
		push_error("cannot save " + name)
	return m

func materials() -> Dictionary:
	var out := {}
	var checker: Texture2D = load("res://checker.png")
	var mask: Texture2D = load("res://mask.png")
	var normal: Texture2D = load("res://normal.png")
	DirAccess.make_dir_recursive_absolute("res://materials")

	# TexTint: checker.png (colour) at uv x 3 times Tint (source_color: its value sRGB, linear 0.8, 0.5, 0.25).
	begin(true)
	var tex := VisualShaderNodeTexture2DParameter.new(); tex.parameter_name = "checker_tex"
	tex.texture_type = VisualShaderNodeTexture2DParameter.TYPE_COLOR
	var t := add(tex)
	var sample := VisualShaderNodeTexture.new(); sample.source = VisualShaderNodeTexture.SOURCE_PORT
	var s := add(sample); link(uv_times(3.0), 0, s, 0); link(t, 0, s, 2)
	var tint := VisualShaderNodeColorParameter.new(); tint.parameter_name = "tint"
	tint.default_value_enabled = true; tint.default_value = Color(0.8, 0.5, 0.25).linear_to_srgb()
	var c := add(tint)
	var mul := VisualShaderNodeVectorOp.new(); mul.op_type = VisualShaderNodeVectorOp.OP_TYPE_VECTOR_4D
	mul.operator = VisualShaderNodeVectorOp.OP_MUL
	var m := add(mul); link(s, 0, m, 0); link(c, 0, m, 1); link(m, 0, 0, 0)
	out["TexTint"] = material("TexTint", {"checker_tex": checker})

	# UVMath: (fract(uv.x * 4), uv.y, sin(uv.x * 2 pi) * 0.5 + 0.5).
	begin(true)
	var uv := split(input("uv"), VisualShaderNodeVectorDecompose.OP_TYPE_VECTOR_2D)
	var fr := ffunc(VisualShaderNodeFloatFunc.FUNC_FRACT, fop(VisualShaderNodeFloatOp.OP_MUL, uv, fconst(4.0)))
	# fop links port 0 of its inputs: uv.x is the decompose's port 0.
	var sn := ffunc(VisualShaderNodeFloatFunc.FUNC_SIN, fop(VisualShaderNodeFloatOp.OP_MUL, uv, fconst(6.2831853)))
	var wave := fop(VisualShaderNodeFloatOp.OP_ADD, fop(VisualShaderNodeFloatOp.OP_MUL, sn, fconst(0.5)), fconst(0.5))
	link(compose(fr, 0, uv, 1, wave, 0), 0, 0, 0)
	out["UVMath"] = material("UVMath")

	# LerpSmooth: lerp(red, blue, smoothstep(0.3, 0.7, uv.x)) + step(0.5, uv.y) * 0.1.
	begin(true)
	uv = split(input("uv"), VisualShaderNodeVectorDecompose.OP_TYPE_VECTOR_2D)
	var ss := add(VisualShaderNodeSmoothStep.new())
	link(fconst(0.3), 0, ss, 0); link(fconst(0.7), 0, ss, 1); link(uv, 0, ss, 2)
	var mix := VisualShaderNodeMix.new(); mix.op_type = VisualShaderNodeMix.OP_TYPE_VECTOR_3D_SCALAR
	var mx := add(mix)
	link(v3const(Vector3(0.9, 0.1, 0.05)), 0, mx, 0); link(v3const(Vector3(0.05, 0.3, 0.9)), 0, mx, 1); link(ss, 0, mx, 2)
	var st := add(VisualShaderNodeStep.new()); link(fconst(0.5), 0, st, 0); link(uv, 1, st, 1)
	var lift := fop(VisualShaderNodeFloatOp.OP_MUL, st, fconst(0.1))
	var sum := VisualShaderNodeVectorOp.new(); sum.operator = VisualShaderNodeVectorOp.OP_ADD
	var su := add(sum); link(mx, 0, su, 0); link(lift, 0, su, 1); link(su, 0, 0, 0)
	out["LerpSmooth"] = material("LerpSmooth")

	# DataMask: remap(mask R, 0..1 -> 0.2..0.8) times (1, 1 - mask G, 1) (data: linear).
	begin(true)
	var mp := VisualShaderNodeTexture2DParameter.new(); mp.parameter_name = "mask_tex"
	mp.texture_type = VisualShaderNodeTexture2DParameter.TYPE_DATA
	var mt := add(mp)
	var ms := VisualShaderNodeTexture.new(); ms.source = VisualShaderNodeTexture.SOURCE_PORT
	var msi := add(ms); link(mt, 0, msi, 2)
	var parts := split(msi, VisualShaderNodeVectorDecompose.OP_TYPE_VECTOR_4D)
	var rm := add(VisualShaderNodeRemap.new())
	link(parts, 0, rm, 0); link(fconst(0.0), 0, rm, 1); link(fconst(1.0), 0, rm, 2); link(fconst(0.2), 0, rm, 3)
	link(fconst(0.8), 0, rm, 4)
	var g := VisualShaderNodeFloatFunc.new(); g.function = VisualShaderNodeFloatFunc.FUNC_ONEMINUS
	var gi := add(g); link(parts, 1, gi, 0)
	link(compose(rm, 0, fop(VisualShaderNodeFloatOp.OP_MUL, rm, gi), 0, rm, 0), 0, 0, 0)
	out["DataMask"] = material("DataMask", {"mask_tex": mask})

	# The lit ones: constants into Albedo (0), Metallic (2), Roughness (3); a normal map (9); emission (5).
	for spec in [["Plain", Vector3(0.8, 0.8, 0.8), 0.0, 0.5], ["Gold", Vector3(1.0, 0.766, 0.336), 1.0, 0.3],
			["Bumps", Vector3(0.6, 0.6, 0.6), 0.0, 0.4], ["Rim", Vector3(0.05, 0.1, 0.3), 0.0, 0.6]]:
		begin(false)
		link(v3const(spec[1]), 0, 0, 0)
		link(fconst(spec[2]), 0, 0, 2)
		link(fconst(spec[3]), 0, 0, 3)
		if spec[0] == "Bumps":
			var nm := VisualShaderNodeTexture.new(); nm.texture = normal
			nm.texture_type = VisualShaderNodeTexture.TYPE_NORMAL_MAP
			var n := add(nm); link(uv_times(4.0), 0, n, 0); link(n, 0, 0, 9)
		if spec[0] == "Rim":
			var fr2 := add(VisualShaderNodeFresnel.new()); link(fconst(3.0), 0, fr2, 3)
			var e := VisualShaderNodeVectorOp.new(); e.operator = VisualShaderNodeVectorOp.OP_MUL
			var ei := add(e); link(fr2, 0, ei, 0); link(v3const(Vector3(1, 0.5, 0.1)), 0, ei, 1); link(ei, 0, 0, 5)
		out[spec[0]] = material(spec[0])
	return out

var viewport: SubViewport
var frames := 0
var out_path := ""

func _initialize() -> void:
	var args := OS.get_cmdline_user_args()
	out_path = args[args.find("--out") + 1] if args.has("--out") else "user://lineup.png"
	var scene: Dictionary = JSON.parse_string(FileAccess.get_file_as_string("res://lineup.json"))
	var mats := materials()
	viewport = SubViewport.new()
	viewport.size = Vector2i(scene.size[0], scene.size[1])
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	viewport.msaa_3d = Viewport.MSAA_DISABLED
	viewport.screen_space_aa = Viewport.SCREEN_SPACE_AA_DISABLED
	viewport.use_taa = false
	viewport.use_debanding = false
	viewport.own_world_3d = true
	root.add_child(viewport)
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0, 0, 0)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0, 0, 0)
	env.ambient_light_energy = 0.0
	env.reflected_light_source = Environment.REFLECTION_SOURCE_DISABLED
	env.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	env.tonemap_exposure = 1.0
	env.glow_enabled = false
	var we := WorldEnvironment.new(); we.environment = env; viewport.add_child(we)
	var cam := Camera3D.new()
	cam.fov = scene.camera.fov_y
	cam.keep_aspect = Camera3D.KEEP_HEIGHT
	viewport.add_child(cam)
	cam.look_at_from_position(Vector3(scene.camera.eye[0], scene.camera.eye[1], scene.camera.eye[2]),
		Vector3(scene.camera.target[0], scene.camera.target[1], scene.camera.target[2]))
	cam.current = true
	var sun := DirectionalLight3D.new()
	var toward := Vector3(scene.sun.toward[0], scene.sun.toward[1], scene.sun.toward[2]).normalized()
	sun.light_energy = scene.sun.intensity
	sun.light_specular = 1.0  # Godot's default 0.5 halves the specular; kin and Unreal have no such factor
	sun.shadow_enabled = false
	viewport.add_child(sun)
	sun.look_at_from_position(Vector3.ZERO, -toward, Vector3.UP if abs(toward.y) < 0.99 else Vector3.FORWARD)
	for o in scene.objects:
		var packed: PackedScene = load("res://%s.glb" % o.mesh)
		var inst := packed.instantiate()
		viewport.add_child(inst)
		inst.position = Vector3(o.position[0], o.position[1], o.position[2])
		for mi in inst.find_children("*", "MeshInstance3D", true, false):
			(mi as MeshInstance3D).material_override = mats[o.material]
			(mi as MeshInstance3D).cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF

func _process(_delta: float) -> bool:
	frames += 1
	if frames == 30:
		var image := viewport.get_texture().get_image()
		image.save_png(out_path)
		print("lineup written: ", out_path, " ", image.get_size())
		return true
	return false
