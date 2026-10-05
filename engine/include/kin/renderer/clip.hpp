#pragma once

// A clip as a value (Renderer2D::push_clip(clip)): for code that hands clips
// on before they are drawn, as RenderQueue groups and the ECS's ClipGroup do.

#include <kin/core/affine.hpp>
#include <kin/renderer/mask.hpp>
#include <kin/renderer/path.hpp>
#include <kin/renderer/texture.hpp>

#include <functional>
#include <memory>
#include <optional>

namespace kin {

class Renderer2D;

// A path clip's edge: anti-aliased (Smooth), or each pixel all in or all out
// (Hard: on SDL_GPU drawn into the stencil buffer, without layers; cheaper).
enum class ClipEdge : u8 { Smooth, Hard };

struct ClipRegion {
    enum class Kind : u8 {
        None, // cuts nothing
        Path, // inside `path`
        Mask, // where `mask` draws, read as `options` say
    };
    Kind kind = Kind::None;
    std::shared_ptr<const Path> path;
    FillRule rule = FillRule::NonZero;
    ClipEdge edge = ClipEdge::Smooth;
    std::function<void(Renderer2D&)> mask;
    MaskOptions options{};
    std::optional<Rectf> extent; // a mask's drawing lies within it, when known

    static ClipRegion to_rect(Rectf rect, ClipEdge edge = ClipEdge::Smooth);
    static ClipRegion to_path(Path path, FillRule rule = FillRule::NonZero, ClipEdge edge = ClipEdge::Smooth);
    static ClipRegion to_mask(std::function<void(Renderer2D&)> draw, MaskOptions options = {},
                              std::optional<Rectf> extent = std::nullopt);
    // A texture stretched over `dest`.
    static ClipRegion to_texture(Texture texture, Rectf dest, MaskOptions options = {});

    explicit operator bool() const { return kind != Kind::None; }
    // The box nothing outside of shows through it, when there is one.
    std::optional<Rectf> bounds() const;
    // Mapped by `transform`: the path moved, the mask drawn under it.
    ClipRegion transformed(const Affine2& transform) const;
};

} // namespace kin
