#include <algorithm>
#include <cmath>
#include <kin/anim/skeleton2d.hpp>
#include <kin/core/json_value.hpp>
#include <set>
#include <stdexcept>

namespace kin {
namespace {
constexpr f32 degrees = 180.0f / 3.14159265358979323846f;
bool finite(Vec2f v) {
    return std::isfinite(v.x) && std::isfinite(v.y);
}
bool finite(const BoneTransform2D& t) {
    return finite(t.position) && finite(t.scale) && std::isfinite(t.rotation);
}
Affine2 matrix(const BoneTransform2D& t) {
    return Affine2::trs(t.position, t.rotation, t.scale);
}
f32 angle(f32 a, f32 b, f32 t) {
    return a + std::remainder(b - a, 360.0f) * t;
}
BoneTransform2D mix(const BoneTransform2D& a, const BoneTransform2D& b, f32 t) {
    return {{a.position.x + (b.position.x - a.position.x) * t, a.position.y + (b.position.y - a.position.y) * t},
            angle(a.rotation, b.rotation, t),
            {a.scale.x + (b.scale.x - a.scale.x) * t, a.scale.y + (b.scale.y - a.scale.y) * t}};
}
std::vector<Affine2> rest_matrices(const SkeletonAsset2D& a) {
    std::vector<Affine2> out;
    out.reserve(a.bones.size());
    for (const auto& b : a.bones)
        out.push_back((b.parent >= 0 ? out[b.parent] : Affine2{}) * matrix(b.rest));
    return out;
}
} // namespace
bool validate_skeleton2d(const SkeletonAsset2D& a, std::string& error) {
    auto fail = [&](std::string why) {
        error = std::move(why);
        return false;
    };
    if (a.bones.empty() || a.bones.size() > 256 || a.vertices.size() > 65536 || a.indices.size() > 196608 ||
        a.indices.size() % 3)
        return fail("invalid skeleton/mesh size (1..256 bones, at most 65536 vertices)");
    std::set<std::string> names;
    for (std::size_t i = 0; i < a.bones.size(); ++i) {
        const auto& b = a.bones[i];
        if (b.name.empty() || !names.insert(b.name).second || b.parent < -1 || b.parent >= static_cast<i32>(i) ||
            !finite(b.rest) || !std::isfinite(b.length) || b.length < 0)
            return fail("invalid bone or parent order");
    }
    const auto rest = rest_matrices(a);
    for (const auto& m : rest)
        if (!std::isfinite(m.determinant()) || std::abs(m.determinant()) < 1e-12f || !std::isfinite(m.tx) ||
            !std::isfinite(m.ty))
            return fail("singular or overflowing bind pose");
    for (const auto& v : a.vertices) {
        if (!finite(v.position) || !finite(v.uv))
            return fail("invalid vertex");
        f32 total = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            if (!std::isfinite(v.weights[i]) || v.weights[i] < 0 || (v.weights[i] > 0 && v.bones[i] >= a.bones.size()))
                return fail("invalid skin influence");
            total += v.weights[i];
        }
        if (std::abs(total - 1) > 1e-4f)
            return fail("skin weights must sum to one");
    }
    for (auto i : a.indices)
        if (i >= a.vertices.size())
            return fail("mesh index outside vertices");
    names.clear();
    for (const auto& b : a.attachments)
        if (b.name.empty() || !names.insert(b.name).second || b.bone >= a.bones.size() || !finite(b.transform))
            return fail("invalid attachment");
    if (a.clips.size() > 1024)
        return fail("too many clips");
    for (const auto& [name, clip] : a.clips) {
        if (name.empty() || !std::isfinite(clip.duration) || clip.duration <= 0 || clip.tracks.size() > a.bones.size())
            return fail("invalid clip");
        std::set<u32> tracks;
        for (const auto& track : clip.tracks) {
            if (track.bone >= a.bones.size() || !tracks.insert(track.bone).second || track.keys.empty() ||
                track.keys.size() > 65536)
                return fail("invalid bone track");
            f32 previous = -1;
            for (const auto& key : track.keys) {
                if (!std::isfinite(key.time) || key.time < 0 || key.time <= previous || key.time > clip.duration ||
                    !finite(key.transform))
                    return fail("invalid key time/transform");
                previous = key.time;
            }
        }
    }
    error.clear();
    return true;
}
void SkeletonPose2D::reset(const SkeletonAsset2D& a) {
    std::string error;
    if (!validate_skeleton2d(a, error))
        throw std::invalid_argument(error);
    local.clear();
    for (const auto& b : a.bones)
        local.push_back(b.rest);
    evaluate(a);
}
void SkeletonPose2D::evaluate(const SkeletonAsset2D& a) {
    if (local.size() != a.bones.size())
        throw std::invalid_argument("pose size differs from skeleton");
    const auto rest = rest_matrices(a);
    world.resize(local.size());
    skin.resize(local.size());
    for (std::size_t i = 0; i < local.size(); ++i) {
        if (!finite(local[i]))
            throw std::invalid_argument("invalid local pose");
        const auto parent = a.bones[i].parent;
        world[i] = (parent >= 0 ? world[parent] : Affine2{}) * matrix(local[i]);
        skin[i] = world[i] * rest[i].inverse();
    }
}
bool sample_skeleton2d(const SkeletonAsset2D& a, std::string_view name, f32 time, bool loop, SkeletonPose2D& pose) {
    const auto found = a.clips.find(name);
    if (found == a.clips.end() || !std::isfinite(time))
        return false;
    const auto& clip = found->second;
    if (loop) {
        time = std::fmod(time, clip.duration);
        if (time < 0)
            time += clip.duration;
    } else
        time = std::clamp(time, 0.0f, clip.duration);
    pose.local.clear();
    for (const auto& bone : a.bones)
        pose.local.push_back(bone.rest);
    for (const auto& track : clip.tracks) {
        const auto next = std::ranges::upper_bound(track.keys, time, {}, &BoneKey2D::time);
        if (next == track.keys.begin())
            pose.local[track.bone] = next->transform;
        else if (next == track.keys.end())
            pose.local[track.bone] = track.keys.back().transform;
        else {
            const auto& from = *(next - 1);
            pose.local[track.bone] =
                mix(from.transform, next->transform, (time - from.time) / (next->time - from.time));
        }
    }
    pose.evaluate(a);
    return true;
}
std::vector<ShaderVertex> deform_skeleton2d(const SkeletonAsset2D& a, const SkeletonPose2D& pose, Color tint) {
    if (pose.skin.size() != a.bones.size())
        return {};
    std::vector<ShaderVertex> out;
    out.reserve(a.vertices.size());
    for (const auto& v : a.vertices) {
        Vec2f p{};
        for (std::size_t i = 0; i < 4; ++i)
            if (v.weights[i] > 0) {
                const auto q = pose.skin[v.bones[i]].apply(v.position);
                p.x += q.x * v.weights[i];
                p.y += q.y * v.weights[i];
            }
        out.push_back({.position = p, .uv = v.uv, .color = tint});
    }
    return out;
}
std::optional<Affine2> skeleton_attachment2d(const SkeletonAsset2D& a, const SkeletonPose2D& pose,
                                             std::string_view name) {
    const auto found = std::ranges::find(a.attachments, name, &BoneAttachment2D::name);
    if (found == a.attachments.end() || found->bone >= pose.world.size())
        return std::nullopt;
    return pose.world[found->bone] * matrix(found->transform);
}
bool solve_two_bone_ik2d(const SkeletonAsset2D& a, SkeletonPose2D& pose, u32 root, u32 tip, Vec2f target, f32 bend,
                         f32 weight) {
    if (root >= a.bones.size() || tip >= a.bones.size() || a.bones[tip].parent != static_cast<i32>(root) ||
        pose.world.size() != a.bones.size() || !finite(target) || !std::isfinite(bend) || !std::isfinite(weight))
        return false;
    const auto uniform = [](const Affine2& m) { return m.is_similarity() && m.determinant() > 0; };
    const auto local_uniform = [](const BoneTransform2D& t) {
        return t.scale.x > 0 && t.scale.y > 0 && std::abs(t.scale.x - t.scale.y) <= t.scale.x * 1e-5f;
    };
    if (!uniform(pose.world[root]) || !uniform(pose.world[tip]) || !local_uniform(pose.local[root]) ||
        !local_uniform(pose.local[tip]) || (a.bones[root].parent >= 0 && !uniform(pose.world[a.bones[root].parent])))
        return false;
    const auto origin = pose.world[root].apply({}), joint = pose.world[tip].apply({});
    const auto norm = [](Vec2f v) { return std::hypot(v.x, v.y); };
    const f32 l1 = norm({joint.x - origin.x, joint.y - origin.y});
    const f32 l2 = a.bones[tip].length * pose.world[tip].uniform_scale();
    if (l1 < 1e-5f || l2 < 1e-5f)
        return false;
    const auto delta = Vec2f{target.x - origin.x, target.y - origin.y};
    const f32 distance = std::clamp(norm(delta), std::abs(l1 - l2) + 1e-5f, l1 + l2);
    const f32 heading = std::atan2(delta.y, delta.x);
    const f32 cosine = std::clamp((l1 * l1 + distance * distance - l2 * l2) / (2 * l1 * distance), -1.0f, 1.0f);
    const f32 first = heading - (bend >= 0 ? 1.0f : -1.0f) * std::acos(cosine);
    const auto elbow = Vec2f{origin.x + l1 * std::cos(first), origin.y + l1 * std::sin(first)};
    const f32 second = std::atan2(origin.y + distance * std::sin(heading) - elbow.y,
                                  origin.x + distance * std::cos(heading) - elbow.x);
    const f32 root_old = pose.local[root].rotation, tip_old = pose.local[tip].rotation;
    pose.local[root].rotation +=
        std::remainder(first * degrees - std::atan2(joint.y - origin.y, joint.x - origin.x) * degrees, 360.0f);
    pose.evaluate(a);
    pose.local[tip].rotation += std::remainder(second * degrees - pose.world[tip].rotation_degrees(), 360.0f);
    const f32 t = std::clamp(weight, 0.0f, 1.0f);
    pose.local[root].rotation = angle(root_old, pose.local[root].rotation, t);
    pose.local[tip].rotation = angle(tip_old, pose.local[tip].rotation, t);
    pose.evaluate(a);
    return true;
}

std::optional<SkeletonAsset2D> parse_skeleton2d(std::string_view text, std::string& error) {
    try {
        const auto parsed = parse_json(text);
        if (!parsed.ok())
            throw std::runtime_error(parsed.error);
        const auto& doc = *parsed.value;
        if (doc.string_at("schema") != "kin.skeleton2d/1")
            throw std::runtime_error("expected kin.skeleton2d/1");
        auto number = [](const JsonValue& value, std::string_view key, f32 fallback = 0) {
            const auto* n = value.find(key);
            if (!n)
                return fallback;
            if (!n->is_number() || !std::isfinite(n->as_number()) || std::abs(n->as_number()) > 1e15)
                throw std::runtime_error("invalid number");
            return static_cast<f32>(n->as_number());
        };
        auto integer = [&](const JsonValue& value, std::string_view key, i32 fallback = 0) {
            auto n = number(value, key, static_cast<f32>(fallback));
            if (n < -1 || n > 65536 || std::floor(n) != n)
                throw std::runtime_error("invalid bone/index");
            return static_cast<i32>(n);
        };
        auto vector = [&](const JsonValue& value, std::string_view key, Vec2f fallback = Vec2f{}) {
            const auto* v = value.find(key);
            if (!v)
                return fallback;
            if (!v->is_object())
                throw std::runtime_error("expected vector object");
            return Vec2f{number(*v, "x", fallback.x), number(*v, "y", fallback.y)};
        };
        auto transform = [&](const JsonValue& value, BoneTransform2D fallback = BoneTransform2D{}) {
            return BoneTransform2D{vector(value, "position", fallback.position),
                                   number(value, "rotation", fallback.rotation),
                                   vector(value, "scale", fallback.scale)};
        };
        auto array = [](const JsonValue& value, std::string_view key, std::size_t limit) -> const JsonValue::Array& {
            const auto* v = value.find(key);
            if (!v) {
                static const JsonValue::Array empty;
                return empty;
            }
            if (!v->is_array() || v->items().size() > limit)
                throw std::runtime_error("invalid array size/type");
            return v->items();
        };
        SkeletonAsset2D a;
        a.texture_asset = doc.string_at("texture_asset");
        for (const auto& b : array(doc, "bones", 256))
            a.bones.push_back({b.string_at("name"), integer(b, "parent", -1), transform(b), number(b, "length")});
        for (const auto& v : array(doc, "vertices", 65536)) {
            SkinVertex2D vertex;
            vertex.position = vector(v, "position");
            vertex.uv = vector(v, "uv");
            vertex.weights = {};
            const auto& influences = array(v, "influences", 4);
            if (influences.empty())
                throw std::runtime_error("vertex requires influences");
            f32 sum = 0;
            for (std::size_t i = 0; i < influences.size(); ++i) {
                vertex.bones[i] = static_cast<u32>(integer(influences[i], "bone"));
                vertex.weights[i] = number(influences[i], "weight");
                sum += vertex.weights[i];
            }
            if (!std::isfinite(sum) || sum <= 0)
                throw std::runtime_error("invalid influence sum");
            for (auto& w : vertex.weights)
                w /= sum;
            a.vertices.push_back(vertex);
        }
        for (const auto& index : array(doc, "indices", 196608)) {
            const f64 n = index.as_number(-1);
            if (n < 0 || n > 65535 || std::floor(n) != n)
                throw std::runtime_error("invalid mesh index");
            a.indices.push_back(static_cast<u32>(n));
        }
        for (const auto& b : array(doc, "attachments", 4096))
            a.attachments.push_back({b.string_at("name"), static_cast<u32>(integer(b, "bone")), transform(b)});
        if (const auto* clips = doc.find("clips")) {
            if (!clips->is_object() || clips->members().size() > 1024)
                throw std::runtime_error("invalid clips");
            for (const auto& [name, value] : clips->members()) {
                SkeletonClip2D clip;
                clip.duration = number(value, "duration", 1);
                for (const auto& t : array(value, "tracks", 256)) {
                    BoneTrack2D track;
                    track.bone = static_cast<u32>(integer(t, "bone"));
                    if (track.bone >= a.bones.size())
                        throw std::runtime_error("invalid track bone");
                    for (const auto& key : array(t, "keys", 65536))
                        track.keys.push_back({number(key, "time"), transform(key, a.bones[track.bone].rest)});
                    clip.tracks.push_back(std::move(track));
                }
                a.clips.emplace(name, std::move(clip));
            }
        }
        if (!validate_skeleton2d(a, error))
            return std::nullopt;
        return a;
    } catch (const std::exception& e) {
        error = e.what();
        return std::nullopt;
    }
}
} // namespace kin
