extends RefCounted
# Reads exactly the same baked camera/instance/mesh/texture bytes as Kin. No world copy.
const ROUTE_COUNT = 1440
const FOLIAGE_LIMIT = 100000
const TRANSPARENT_LIMIT = 20000
var route: PackedVector3Array
var foliage_instances = 0
var transparent_instances = 0
var batches: Array[MultiMeshInstance3D] = []
var foliage_multis: Array[MultiMesh] = []
var moving_count = 0
var moving_first = 0
var moving_multis: Array[MultiMesh] = []
var moving_buffers: Array[PackedFloat32Array] = []
var moving_spans: Array[Vector2i] = []
var moving_origins: PackedVector3Array
var motion_upload_bytes = 0

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
    foliage_multis = sets[0]
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

func configure_motion(count: int) -> String:
    moving_count = count
    if count == 0: return ""
    moving_first = mini(foliage_instances - count, foliage_instances * 9 / 16)
    var first = 0
    for multi in foliage_multis:
        var last = first + multi.instance_count
        var begin_edit = maxi(first, moving_first)
        var end_edit = mini(last, moving_first + count)
        if begin_edit < end_edit:
            var data = multi.buffer
            if data.size() != multi.instance_count * 12: return "Unexpected MultiMesh transform buffer layout"
            moving_multis.append(multi)
            moving_buffers.append(data)
            moving_spans.append(Vector2i(begin_edit - first, end_edit - begin_edit))
            motion_upload_bytes += data.size() * 4
            for i in range(begin_edit - first, end_edit - first):
                var origin = multi.get_instance_transform(i).origin
                if origin != Vector3(data[i * 12 + 3], data[i * 12 + 7], data[i * 12 + 11]): return "MultiMesh buffer origin mismatch"
                moving_origins.append(origin)
        first = last
    return "" if moving_origins.size() == count else "Missing moving instances"

func animate_motion(index: int) -> void:
    if moving_count == 0: return
    # Match native float phase/offset storage. Packed buffers submit each dirty
    # cell in bulk, retaining its unchanged transforms and original instance IDs.
    var phase = PackedFloat32Array([float(index) * PackedFloat32Array([0.025])[0]])[0]
    var offset = Vector2(4.0 * sin(phase), 2.0 * (1.0 - cos(phase)))
    var record = 0
    for b in moving_multis.size():
        var data = moving_buffers[b]
        var span = moving_spans[b]
        for i in range(span.x, span.x + span.y):
            var origin = moving_origins[record]
            data[i * 12 + 3] = origin.x + offset.x
            data[i * 12 + 7] = origin.y + offset.y
            data[i * 12 + 11] = origin.z
            record += 1
        moving_multis[b].buffer = data
        moving_buffers[b] = data

func motion_sums() -> Dictionary:
    var sums = {"moving_base_x_sum":0.0,"moving_base_y_sum":0.0,"moving_base_z_sum":0.0,"moving_x_sum":0.0,"moving_y_sum":0.0,"moving_z_sum":0.0}
    var record = 0
    for b in moving_multis.size():
        var span = moving_spans[b]
        for i in range(span.x, span.x + span.y):
            var origin = moving_origins[record]
            var current = moving_multis[b].get_instance_transform(i).origin
            sums["moving_base_x_sum"] += origin.x; sums["moving_base_y_sum"] += origin.y; sums["moving_base_z_sum"] += origin.z
            sums["moving_x_sum"] += current.x; sums["moving_y_sum"] += current.y; sums["moving_z_sum"] += current.z
            record += 1
    return sums

static func read_packed(file: FileAccess, type: int, count: int, stride: int) -> Variant:
    var payload = file.get_buffer(count * stride)
    if payload.size() != count * stride: return null
    var header = PackedByteArray()
    header.resize(8)
    header.encode_u32(0, type)
    header.encode_u32(4, count)
    header.append_array(payload)
    return bytes_to_var(header)
