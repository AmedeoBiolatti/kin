#include "scene_components_internal.hpp"
#include "scene_components_blend.hpp"

#include <kin/assets/asset_manager.hpp>
#include <kin/ecs/camera.hpp>
#include <kin/ecs/render.hpp>
#include <kin/ecs/text.hpp>

#include <optional>
#include <string>

namespace kin::scene_detail {

void register_scene_view_components(EcsWorld& world) {
    if (!world.components().find("CameraComponent")) {
        // What the game sees of its world: EcsScene draws through the first
        // current one, looking at where its entity is (kin/ecs/camera.hpp).
        world.components().native<CameraComponent>("CameraComponent")
            .field("current", &CameraComponent::current)
            .description("The one the game is drawn through (the first current one).")
            .field("zoom", &CameraComponent::zoom)
            .range(0.01, 100.0)
            .description("2: the world twice as big.")
            .field("smoothing", &CameraComponent::smoothing)
            .units("seconds")
            .range(0.0, 10.0)
            .description("How long it takes to catch its entity up (update_cameras); 0: at once.")
            .field("limited", &CameraComponent::limited)
            .description("It shows nothing past its limits.")
            .field("limit_min", &CameraComponent::limit_min)
            .description("The world's top-left corner it shows no further than.")
            .field("limit_max", &CameraComponent::limit_max)
            .description("And its bottom-right.")
            .field("dead_zone", &CameraComponent::dead_zone)
            .units("points")
            .description("A box its entity moves about in without the camera following; 0: none.")
            .field("look_ahead", &CameraComponent::look_ahead)
            .units("points")
            .description("How far ahead it looks the way its entity moves, along each axis; 0: none.")
            .field("look_ahead_time", &CameraComponent::look_ahead_time)
            .units("seconds")
            .range(0.0, 5.0)
            .description("How many seconds of its entity's motion it looks ahead (no further than look_ahead).")
            .field("zoom_smoothing", &CameraComponent::zoom_smoothing)
            .units("seconds")
            .range(0.0, 10.0)
            .description("How long a change of zoom takes; 0: at once.")
            .field("shake_strength", &CameraComponent::shake_strength)
            .units("points")
            .range(0.0, 1000.0)
            .description("How far it moves at full trauma (shake_camera, a script's camera.shake).")
            .field("shake_frequency", &CameraComponent::shake_frequency)
            .units("/s")
            .range(0.0, 200.0)
            .description("How many times a second it jitters.")
            .field("shake_decay", &CameraComponent::shake_decay)
            .units("/s")
            .range(0.0, 100.0)
            .description("How much trauma wears off a second (1: full trauma gone in a second).")
            .field("trauma", &CameraComponent::trauma)
            .readonly()
            .description("Its trauma now, 0 to 1: it shakes by its square.");
    }
    if (!world.components().find("ShapeRenderer")) {
        // A vector shape at the entity: a box, a rounded box, an ellipse, a
        // regular polygon, or an SVG file (Assets2D makes its mesh).
        auto shape = world.components().native<ShapeRenderer>("ShapeRenderer");
        shape.property<std::string>("shape", [](const ShapeRenderer& r) { return std::string(shape_kind_name(r.spec.kind)); },
                                   [](ShapeRenderer& r, const std::string& name) {
                                       const std::optional<ShapeKind> kind = shape_kind_named(name);
                                       if (!kind) return false;
                                       r.spec.kind = *kind;
                                       return true;
                                   })
            .choices({"none", "rect", "rounded_rect", "ellipse", "polygon", "svg"})
            .description("none: the game sets its mesh; svg: the file `asset` names.")
            .field("asset", &ShapeRenderer::asset)
            .asset_type(std::string{asset_type_name(AssetType::Shape)})
            .description("An SVG file of the content (shape svg).")
            .field("size", &ShapeRenderer::spec, &ShapeSpec::size)
            .description("Centred on the entity; an SVG is fitted in it (zero: its own size).")
            .field("radius", &ShapeRenderer::spec, &ShapeSpec::radius)
            .description("A rounded box's corners.")
            .field("sides", &ShapeRenderer::spec, &ShapeSpec::sides)
            .range(3.0, 256.0)
            .description("A polygon's.")
            .field("fill", &ShapeRenderer::spec, &ShapeSpec::fill)
            .field("stroke", &ShapeRenderer::spec, &ShapeSpec::stroke)
            .description("Its outline's colour.")
            .field("stroke_width", &ShapeRenderer::spec, &ShapeSpec::stroke_width)
            .range(0.0, 1000.0)
            .field("offset", &ShapeRenderer::offset)
            .field("tint", &ShapeRenderer::tint)
            .description("Multiplies its colours (an SVG's too).")
            .field("layer", &ShapeRenderer::layer)
            .field("order", &ShapeRenderer::order)
            .field("y_sort", &ShapeRenderer::y_sort)
            .field("visible", &ShapeRenderer::visible);
        describe_blend(shape);
    }
    if (!world.components().find("ClipGroup")) {
        // Its entity's renderers, and those of every entity under it, cut to a
        // shape that moves, turns and scales with it.
        const auto clip_field = [](auto member) {
            return [member](ClipGroup& group, const auto& value) {
                group.*member = value;
                reshape_clip(group);
                return true;
            };
        };
        world.components().native<ClipGroup>("ClipGroup")
            .property<std::string>("shape",
                                   [](const ClipGroup& g) {
                                       switch (g.shape) {
                                       case ClipShape::None: return std::string("none");
                                       case ClipShape::Rect: return std::string("rect");
                                       case ClipShape::RoundedRect: return std::string("rounded_rect");
                                       case ClipShape::Ellipse: return std::string("ellipse");
                                       }
                                       return std::string("none");
                                   },
                                   [](ClipGroup& g, const std::string& name) {
                                       if (name == "rect") g.shape = ClipShape::Rect;
                                       else if (name == "rounded_rect") g.shape = ClipShape::RoundedRect;
                                       else if (name == "ellipse") g.shape = ClipShape::Ellipse;
                                       else if (name == "none" || name.empty()) g.shape = ClipShape::None;
                                       else return false;
                                       reshape_clip(g);
                                       return true;
                                   })
            .choices({"none", "rect", "rounded_rect", "ellipse"})
            .description("What shows of them: what is inside it (none: the game sets its clip).")
            .property<Vec2f>("size", [](const ClipGroup& g) { return g.size; }, clip_field(&ClipGroup::size))
            .property<Vec2f>("offset", [](const ClipGroup& g) { return g.offset; }, clip_field(&ClipGroup::offset))
            .description("Its centre, from the entity.")
            .property<f32>("radius", [](const ClipGroup& g) { return g.radius; }, clip_field(&ClipGroup::radius))
            .description("A rounded box's corners.")
            .field("layer", &ClipGroup::layer)
            .description("They are drawn together, at its layer and order.")
            .field("order", &ClipGroup::order)
            .field("y_sort", &ClipGroup::y_sort);
    }
    if (!world.components().find("TextRenderer")) {
        // Text in the world at the entity, in a font of the content (Assets2D
        // reads it) or the system's.
        world.components().native<TextRenderer>("TextRenderer")
            .field("text", &TextRenderer::text)
            .field("key", &TextRenderer::key)
            .description("A translation key; empty: scene.<scene>.<entity>, where a language has it.")
            .field("font", &TextRenderer::font)
            .asset_type(std::string{asset_type_name(AssetType::Font)})
            .description("A font file of the content; empty: the system's.")
            .field("size", &TextRenderer::size)
            .units("points")
            .range(1.0, 512.0)
            .field("color", &TextRenderer::color)
            .field("offset", &TextRenderer::offset)
            .field("pivot", &TextRenderer::pivot)
            .description("The point of its box at the entity: 0.5, 0.5 its centre.")
            .field("wrap", &TextRenderer::wrap)
            .description("Lines break past this wide (0: only at line ends).")
            .property<std::string>("align", [](const TextRenderer& t) { return std::string(text_align_name(t.align)); },
                                   [](TextRenderer& t, const std::string& name) {
                                       const std::optional<TextAlign> align = text_align_named(name);
                                       if (!align) return false;
                                       t.align = *align;
                                       return true;
                                   })
            .choices({"left", "center", "right"})
            .field("outline", &TextRenderer::outline)
            .range(0.0, 64.0)
            .description("Points round each letter.")
            .field("outline_color", &TextRenderer::outline_color)
            .field("layer", &TextRenderer::layer)
            .field("order", &TextRenderer::order)
            .field("y_sort", &TextRenderer::y_sort)
            .field("visible", &TextRenderer::visible);
    }
}

} // namespace kin::scene_detail
