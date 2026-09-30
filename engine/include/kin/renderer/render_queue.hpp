#pragma once

#include <kin/renderer/backend.hpp>
#include <kin/renderer/render_command.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace kin {

// A sprite made ready outside the queue (e.g. on worker threads) for
// RenderQueue::append_sprites().
struct PreparedSprite {
    RenderKey key{};
    const Texture* texture = nullptr; // must stay valid until appended
    Rectf source{};                   // texture pixels; empty: all of it (Texture only)
    Rectf dest{};
    Color tint = colors::white;
    f32 rotation = 0.0f;
    Vec2f pivot{0.5f, 0.5f};
    RenderCommandType type = RenderCommandType::Texture; // or Sprite
};

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
    std::size_t size() const { return _commands.size() + _sprites.size(); }
    std::size_t capacity() const { return std::min(_commands.capacity(), _sprites.capacity()); }
    bool empty() const { return size() == 0; }
    // Every command, in submission order (draw order after sort_commands()).
    // Plain sprites are stored compactly until asked for here, so the first call
    // after sprites were submitted converts them.
    std::span<const RenderCommand> commands() const {
        materialize();
        return _commands;
    }
    // Commands submitted since clear(), a mark for cull(view, first, last).
    u64 submitted() const { return _next_sequence; }

    void submit(RenderCommand command);
    // Appends copies of `commands` in order, e.g. a cached batch.
    void submit(std::span<const RenderCommand> commands);
    void clear_color(Color color);
    void fill_rect(RenderKey key, Rectf rect, Color color, MaterialRef material = {});
    void draw_rect(RenderKey key, Rectf rect, Color color);
    void draw_line(RenderKey key, Vec2f a, Vec2f b, Color color);
    void draw_texture(RenderKey key, const Texture& texture, Rectf dest, Color tint = colors::white, MaterialRef material = {}, f32 rotation = 0.0f, Vec2f pivot = {0.5f, 0.5f});
    void draw_sprite(RenderKey key, const Sprite& sprite, Rectf dest, Color tint = colors::white, MaterialRef material = {}, f32 rotation = 0.0f, Vec2f pivot = {0.5f, 0.5f});
    // Queues each sprite in order, as draw_texture_region() (Texture) or
    // draw_sprite() (Sprite) would.
    void append_sprites(std::span<const PreparedSprite> sprites);

    // For sprites produced on several threads, without copying them twice: room
    // for `count` sprites, queued in order as if submitted one by one now. Register
    // their textures with texture_index() (one thread), then fill every slot
    // exactly once with write_sprite() (any threads, distinct slots) before the
    // queue is used again.
    struct SpriteBlock {
        std::size_t first = 0;
        u64 sequence = 0;
        std::size_t count = 0;
    };
    SpriteBlock reserve_sprites(std::size_t count);
    u32 texture_index(const Texture& texture) { return texture_slot(texture); }
    // `sprite` must be drawable: its texture valid (and registered as `texture`),
    // and a source given for a Sprite.
    void write_sprite(const SpriteBlock& block, std::size_t index, u32 texture, const PreparedSprite& sprite);
    // A Texture command drawing `source` (texture pixels) of `texture`.
    void draw_texture_region(RenderKey key, const Texture& texture, Rectf source, Rectf dest, Color tint = colors::white, f32 rotation = 0.0f, Vec2f pivot = {0.5f, 0.5f});
    void draw_text(RenderKey key, std::string text, Vec2f pos, Rectf bounds, f32 scale, Color color, std::function<void(Renderer2D&)> callback);
    void push_viewport(Rectf rect);
    void pop_viewport();
    void custom(RenderKey key, std::function<void(Renderer2D&)> callback, std::string debug_name = {});

    // Reorders the commands into draw order, so commands() and the presorted
    // flushes see them sorted. Does nothing when nothing changed since the last sort.
    void sort_commands();
    void cull(const RenderView& view);
    // Culls only the commands submitted in [first, last), counted as submitted()
    // does, e.g. the ones appended after submissions already culled one by one.
    void cull(const RenderView& view, u64 first, u64 last = ~u64{0});
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
    // A plain Texture or Sprite command (a valid texture, no material, not in
    // output pixels), kept compactly: the texture is an index into _textures, so
    // queueing a sprite copies no texture handle.
    // Trivial (every field is set where one is made), so reserve_sprites() can
    // grow the list without initializing slots the writers fill anyway.
    struct QueuedSprite {
        Rectf dest;
        Rectf source;
        Color tint;
        f32 rotation;
        Vec2f pivot;
        i32 layer;
        i32 order;
        f32 y;
        u32 texture;
        u64 pass_mask;
        u64 sequence;
        bool use_y;
        RenderCommandType type;
    };
    // Default-initializes on resize(): for trivial types, leaves memory as is.
    template<typename T>
    struct UninitializedAllocator : std::allocator<T> {
        template<typename U>
        struct rebind {
            using other = UninitializedAllocator<U>;
        };
        using std::allocator<T>::allocator;
        template<typename U>
        void construct(U* p) noexcept {
            ::new (static_cast<void*>(p)) U;
        }
        template<typename U, typename... Args>
        void construct(U* p, Args&&... args) {
            ::new (static_cast<void*>(p)) U(std::forward<Args>(args)...);
        }
    };

    bool before(const RenderCommand& a, const RenderCommand& b) const;
    // Leaves _sort_keys holding the draw order (entry i is the i-th command to
    // draw; see SortEntry::index). Returns false when that is already the
    // physical order of _commands (only possible with no queued sprites).
    bool compute_draw_order();
    // Calls draw(command, sprite) for each command in draw order: exactly one of
    // the two pointers is set.
    template<typename Draw>
    void for_each_in_draw_order(Draw&& draw);
    template<typename Visit>
    void for_each_in_submission_order(Visit&& visit) const;
    void queue_sprite(RenderCommandType type, RenderKey key, const Texture& texture, Rectf source, Rectf dest,
                      Color tint, f32 rotation, Vec2f pivot);
    u32 texture_slot(const Texture& texture);
    RenderCommand to_command(const QueuedSprite& sprite) const;
    // Moves the queued sprites into _commands, in submission order.
    void materialize() const;

    // Sort key with each field encoded as an unsigned integer that orders like the
    // original, so keys can be radix sorted. Ties break by submission sequence.
    struct SortEntry {
        u32 order;
        u32 y;
        u32 layer;
        u32 index; // position in _commands, or in _sprites with sprite_bit set
    };
    static constexpr u32 sprite_bit = 0x8000'0000u;

    RenderSortMode _sort = RenderSortMode::LayerThenOrder;
    u64 _next_sequence = 0;
    // Mutable: commands() and the presorted flushes materialize queued sprites.
    mutable std::vector<RenderCommand> _commands;
    mutable std::vector<QueuedSprite, UninitializedAllocator<QueuedSprite>> _sprites;
    mutable std::vector<Texture> _textures; // referenced by _sprites
    mutable std::size_t _sprites_after = 0; // _commands before this index predate every queued sprite
    std::array<u32, 64> _texture_cache{};   // slot + 1 by texture address; checked, so never stale
    bool _sorted = true;      // _commands is in draw order for _sort
    bool _in_sequence = true; // _commands is in submission order
    std::vector<SortEntry> _sort_keys;        // reused across frames (capacity retained)
    std::vector<SortEntry> _radix_scratch;    // radix sort ping-pong buffer
    mutable std::vector<RenderCommand> _sort_scratch; // permutation/materialize target, reused across frames
    // Consecutive same-texture sprites gathered during a flush, drawn with one
    // draw_sprites() call; reused across flushes.
    mutable std::vector<SpriteInstance> _sprite_run;
};

} // namespace kin
