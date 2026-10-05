#include <kin/renderer/clip.hpp>

#include <kin/renderer/renderer2d.hpp>

#include <utility>

namespace kin {

ClipRegion ClipRegion::to_rect(Rectf rect, ClipEdge edge) {
    return to_path(Path::rect(rect), FillRule::NonZero, edge);
}

ClipRegion ClipRegion::to_path(Path path, FillRule rule, ClipEdge edge) {
    return {.kind = Kind::Path, .path = std::make_shared<const Path>(std::move(path)), .rule = rule, .edge = edge};
}

ClipRegion ClipRegion::to_mask(std::function<void(Renderer2D&)> draw, MaskOptions options, std::optional<Rectf> extent) {
    return {.kind = Kind::Mask, .mask = std::move(draw), .options = options, .extent = extent};
}

ClipRegion ClipRegion::to_texture(Texture texture, Rectf dest, MaskOptions options) {
    return to_mask([texture = std::move(texture), dest](Renderer2D& renderer) { renderer.draw_texture(texture, dest); },
                   options, dest);
}

std::optional<Rectf> ClipRegion::bounds() const {
    switch (kind) {
    case Kind::Path: return path ? std::optional<Rectf>{path->bounds()} : std::nullopt;
    case Kind::Mask: return options.invert ? std::nullopt : extent;
    case Kind::None: break;
    }
    return std::nullopt;
}

ClipRegion ClipRegion::transformed(const Affine2& transform) const {
    ClipRegion out = *this;
    if (kind == Kind::Path && path) {
        out.path = std::make_shared<const Path>(path->transformed(transform));
    } else if (kind == Kind::Mask && mask) {
        out.mask = [draw = mask, transform](Renderer2D& renderer) {
            const auto under = renderer.scoped_transform(transform);
            draw(renderer);
        };
        if (extent) {
            out.extent = transformed_bounds(transform, *extent);
        }
    }
    return out;
}

} // namespace kin
