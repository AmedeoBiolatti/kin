extends RefCounted
# Reads exactly the same baked camera/instance/mesh/texture bytes as Kin. No world copy.
const ROUTE_COUNT = 1440
const FOLIAGE_LIMIT = 100000
const TRANSPARENT_LIMIT = 20000
var route: PackedVector3Array
var foliage_instances = 0
var transparent_instances = 0
var batches: Array[MultiMeshInstance3D] = []

func load_recipe(path: String, foliage: int, transparent: int, viewport: SubViewport, shadows: bool, unlit: bool) -> String:
    var file = FileAccess.open(path, FileAccess.READ)
    if file == null or file.get_32() != 0x31444642 or file.get_32() != 1 or file.get_32() != ROUTE_COUNT or file.get_32() != FOLIAGE_LIMIT or file.get_32() != TRANSPARENT_LIMIT:
        return "Invalid grove recipe header"
    route = read_packed(file, TYPE_PACKED_VECTOR3_ARRAY, ROUTE_COUNT * 2, 12)
    if route.size() != ROUTE_COUNT * 2: return "Truncated grove route"
    var sets: Array = []
    for kind in 2:
        var count = transparent if kind else foliage
        var groups = 8 if kind else 64
        var multis: Array[MultiMesh] = []
        for group in groups:
            var multi = MultiMesh.new()
            multi.transform_format = MultiMesh.TRANSFORM_3D
            multi.instance_count = count / groups + (1 if group < count % groups else 0)
            multis.append(multi)
        for i in count:
            var p = Vector3(file.get_float(), file.get_float(), file.get_float())
            var yaw = file.get_float()
            var scale = Vector3(file.get_float(), file.get_float(), file.get_float())
            if not p.is_finite() or not scale.is_finite() or not is_finite(yaw) or scale.x <= 0 or scale.y <= 0 or scale.z <= 0 or scale.length() > 100:
                return "Invalid grove transform"
            multis[i % groups].set_instance_transform(i / groups, Transform3D((Basis(Vector3.UP, deg_to_rad(yaw)) * Basis.from_scale(scale)), p))
        file.seek(file.get_position() + ((TRANSPARENT_LIMIT if kind else FOLIAGE_LIMIT) - count) * 28)
        sets.append(multis)
    for kind in 2:
        var nv = file.get_32()
        var ni = file.get_32()
        var side = file.get_32()
        if nv < 1 or nv > 64 or ni < 1 or ni > 128 or ni % 3 or side not in [64,128]: return "Invalid grove mesh"
        var arrays = []
        arrays.resize(Mesh.ARRAY_MAX)
        arrays[Mesh.ARRAY_VERTEX] = read_packed(file, TYPE_PACKED_VECTOR3_ARRAY, nv, 12)
        arrays[Mesh.ARRAY_NORMAL] = read_packed(file, TYPE_PACKED_VECTOR3_ARRAY, nv, 12)
        arrays[Mesh.ARRAY_TEX_UV] = read_packed(file, TYPE_PACKED_VECTOR2_ARRAY, nv, 8)
        var indices: PackedInt32Array = read_packed(file, TYPE_PACKED_INT32_ARRAY, ni, 4)
        if indices.size() != ni: return "Truncated grove indices"
        for i in range(0, ni, 3):
            var temp = indices[i + 1]
            indices[i + 1] = indices[i + 2]
            indices[i + 2] = temp
        arrays[Mesh.ARRAY_INDEX] = indices
        var bytes = file.get_buffer(side * side * 4)
        if bytes.size() != side * side * 4: return "Truncated grove texture"
        var material = StandardMaterial3D.new()
        material.albedo_texture = ImageTexture.create_from_image(Image.create_from_data(side, side, false, Image.FORMAT_RGBA8, bytes))
        material.texture_filter = BaseMaterial3D.TEXTURE_FILTER_LINEAR
        material.cull_mode = BaseMaterial3D.CULL_DISABLED
        material.roughness = 1.0
        if kind:
            material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
            material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
        else:
            material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA_SCISSOR
            material.alpha_scissor_threshold = 0.45
            material.diffuse_mode = BaseMaterial3D.DIFFUSE_LAMBERT
            if unlit: material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
        var mesh = ArrayMesh.new()
        mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
        mesh.surface_set_material(0, material)
        for multi in sets[kind]:
            if multi.instance_count == 0: continue
            multi.mesh = mesh
            var instance = MultiMeshInstance3D.new()
            instance.multimesh = multi
            instance.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_ON if shadows and not kind else GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
            viewport.add_child(instance)
            batches.append(instance)
    if file.get_position() != file.get_length(): return "Trailing grove data"
    foliage_instances = foliage
    transparent_instances = transparent
    return ""

static func read_packed(file: FileAccess, type: int, count: int, stride: int) -> Variant:
    var payload = file.get_buffer(count * stride)
    if payload.size() != count * stride: return null
    var header = PackedByteArray()
    header.resize(8)
    header.encode_u32(0, type)
    header.encode_u32(4, count)
    header.append_array(payload)
    return bytes_to_var(header)
