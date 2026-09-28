#pragma once

#include <kin/core/types.hpp>
#include <kin/renderer/color.hpp>
#include <kin/renderer/render_target.hpp> // ScaleMode
#include <kin/renderer/texture.hpp>
#include <kin/ui2/image.hpp>

#include <string_view>
#include <vector>

namespace kin {

class AssetManager;
class Renderer2D;

namespace ui2 {

// Bridges the existing AssetManager image pipeline to ui2 skins — no separate skin
// manager: AssetManager caches/dedupes the CPU `Image` (PNG via SDL_image), these
// helpers upload it to a GPU `Texture` once and wrap it as a `UiNineSlice`. Mirrors
// `load_sprite_catalog(assets, renderer, path)`. Intended for one-time setup (build a
// SkinPack once), so the GPU texture is not re-cached; use `nine_slice` to slice one
// atlas texture into several frames without re-uploading.
//
// The caller's AssetManager must have the Image loader registered
// (`register_default_asset_loaders`).

// Load a PNG through the AssetManager (cached Image) and upload it to a Texture.
Texture load_texture(AssetManager& assets, Renderer2D& renderer, std::string_view path,
                     ScaleMode scale = ScaleMode::Linear);

// Wrap (a sub-rect of) a texture as a nine-slice with uniform margins. An empty/zero
// `source` means the whole texture (the single-frame case); pass a sub-rect to slice an
// atlas texture loaded once via `load_texture`.
UiNineSlice nine_slice(Texture texture, f32 margin, Rectf source = {});

// Convenience: load a whole-PNG-per-frame nine-slice in one call.
UiNineSlice load_nine_slice(AssetManager& assets, Renderer2D& renderer, std::string_view path,
                            f32 margin, ScaleMode scale = ScaleMode::Linear);

// --- Procedural frame builder ---
// Build RGBA pixels for a beveled rounded-rect frame: transparent outside, a beveled
// border ring, and an embossed (vertical-gradient) fill inside. `px` = square texture
// side; `radius` = corner radius in px; `border_w` = border band width in px.
// Reusable for both runtime upload (make_frame_skin) and offline asset baking (save_image).
std::vector<u8> make_frame_rgba(int px, Color border, Color fill, f32 radius, f32 border_w);

// Upload the procedural frame to the renderer and return a ready-to-use nine-slice.
// The nine-slice margin is `radius + 3` so corners are pinned and straight edges stretch.
// Intended for one-time setup (call inside a lazy-init guard).
UiNineSlice make_frame_skin(Renderer2D& renderer, int px,
                            Color border, Color fill, f32 radius, f32 border_w);

} // namespace ui2
} // namespace kin
