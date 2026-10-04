#include <kin/renderer/cached_target.hpp>

#include <kin/renderer/renderer2d.hpp>

namespace kin {

bool CachedTarget::stale(Renderer2D& renderer, Vec2i size, u64 key) {
    if (size.x <= 0 || size.y <= 0 || !renderer.capabilities().render_targets) {
        return false;
    }
    if (!_target.valid() || size != _size) {
        _target = renderer.create_render_target(size, _mode);
        _size = size;
        _valid = false;
    }
    if (_valid && key == _key) {
        ++_reuses;
        return false;
    }
    _key = key;
    _valid = _target.valid();
    ++_redraws;
    return _valid;
}

} // namespace kin
