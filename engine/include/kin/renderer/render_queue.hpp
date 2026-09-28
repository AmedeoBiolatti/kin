#pragma once

#include <kin/renderer/render_command.hpp>

#include <span>
#include <vector>

namespace kin {

class RenderQueue {
public:
    explicit RenderQueue(RenderSortMode sort = RenderSortMode::LayerThenOrder);

    void set_sort(RenderSortMode sort) { _sort = sort; }
    RenderSortMode sort() const { return _sort; }
    void clear();
    void reserve(std::size_t capacity);
    std::size_t size() const { return _commands.size(); }
    std::size_t capacity() const { return _commands.capacity(); }
    bool empty() const { return _commands.empty(); }
    std::span<const RenderCommand> commands() const { return _commands; }

    void submit(RenderCommand command);
    void clear_color(Color color);
    void fill_rect(RenderKey key, Rectf rect, Color color, MaterialRef material = {});
    void draw_rect(RenderKey key, Rectf rect, Color color);
    void draw_line(RenderKey key, Vec2f a, Vec2f b, Color color);
    void draw_texture(RenderKey key, const Texture& texture, Rectf dest, Color tint = colors::white, MaterialRef material = {}, f32 rotation = 0.0f, Vec2f pivot = {0.5f, 0.5f});
    void draw_sprite(RenderKey key, const Sprite& sprite, Rectf dest, Color tint = colors::white, MaterialRef material = {}, f32 rotation = 0.0f, Vec2f pivot = {0.5f, 0.5f});
    void draw_text(RenderKey key, std::string text, Vec2f pos, Rectf bounds, f32 scale, Color color, std::function<void(Renderer2D&)> callback);
    void push_viewport(Rectf rect);
    void pop_viewport();
    void custom(RenderKey key, std::function<void(Renderer2D&)> callback, std::string debug_name = {});

    void sort_commands();
    void cull(const RenderView& view);
    void flush(Renderer2D& renderer);
    void flush(Renderer2D& renderer, u64 pass_mask);
    void flush(Renderer2D& renderer, const RenderView& view);
    void flush(Renderer2D& renderer, const RenderView& view, u64 pass_mask);
    void flush_presorted(Renderer2D& renderer, const RenderView& view, u64 pass_mask = render_pass_mask::all) const;
    void flush_merged_presorted(Renderer2D& renderer,
                                std::span<const RenderCommand> other,
                                const RenderView& view,
                                u64 pass_mask = render_pass_mask::all) const;

private:
    bool before(const RenderCommand& a, const RenderCommand& b) const;

    // Compact sort entry. sort_commands sorts these (cache-resident, ~32B) rather
    // than moving the heavy ~300B RenderCommand objects O(n log n) times, then
    // applies the resulting permutation to _commands once (O(n)). _commands stays
    // the physically-sorted source of truth, so commands()/flush_presorted are
    // unchanged. Holds exactly the fields `before` compares.
    struct SortEntry {
        i32 layer;
        i32 order;
        f32 y;
        bool use_y;
        u64 sequence;
        u32 index;
    };

    RenderSortMode _sort = RenderSortMode::LayerThenOrder;
    u64 _next_sequence = 0;
    std::vector<RenderCommand> _commands;
    std::vector<SortEntry> _sort_keys;        // reused across frames (capacity retained)
    std::vector<RenderCommand> _sort_scratch; // permutation target, reused across frames
};

} // namespace kin
