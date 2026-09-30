#pragma once

#include <kin/renderer/render_command.hpp>

#include <span>
#include <vector>

namespace kin {

class RenderQueue {
public:
    explicit RenderQueue(RenderSortMode sort = RenderSortMode::LayerThenOrder);

    void set_sort(RenderSortMode sort) {
        _sorted = _sorted && sort == _sort;
        _sort = sort;
    }
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

    // Reorders the commands into draw order, so commands() and the presorted
    // flushes see them sorted. Does nothing when nothing changed since the last sort.
    void sort_commands();
    void cull(const RenderView& view);
    // flush draws in sorted order without moving the commands: unless
    // sort_commands() was called, commands() keeps submission order afterwards.
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
    // Leaves _sort_keys holding the draw order (entry i is the i-th command to
    // draw). Returns false when that is already the physical order.
    bool compute_draw_order();
    template<typename Draw>
    void for_each_in_draw_order(Draw&& draw);

    // Sort key with each field encoded as an unsigned integer that orders like the
    // original, so keys can be radix sorted. Ties break by submission sequence.
    struct SortEntry {
        u32 order;
        u32 y;
        u32 layer;
        u32 index; // position in _commands
    };

    RenderSortMode _sort = RenderSortMode::LayerThenOrder;
    u64 _next_sequence = 0;
    std::vector<RenderCommand> _commands;
    bool _sorted = true;      // _commands is in draw order for _sort
    bool _in_sequence = true; // _commands is in submission order
    std::vector<SortEntry> _sort_keys;        // reused across frames (capacity retained)
    std::vector<SortEntry> _radix_scratch;    // radix sort ping-pong buffer
    std::vector<RenderCommand> _sort_scratch; // permutation target, reused across frames
};

} // namespace kin
