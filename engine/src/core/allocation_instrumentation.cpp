#include <kin/core/instrumentation.hpp>

#include <cstdlib>
#include <limits>
#include <new>

#if defined(_MSC_VER)
#include <malloc.h>
#endif

namespace {

void* allocate_bytes(std::size_t size) {
    if (kin::detail::allocation_failure_guard_enabled()) {
        throw std::bad_alloc{};
    }

    const std::size_t requested = size == 0 ? 1 : size;
    void* ptr = std::malloc(requested);
    if (!ptr) {
        throw std::bad_alloc{};
    }
    kin::detail::record_allocation(requested);
    return ptr;
}

void* allocate_aligned_bytes(std::size_t size, std::align_val_t alignment) {
    if (kin::detail::allocation_failure_guard_enabled()) {
        throw std::bad_alloc{};
    }

    const std::size_t requested = size == 0 ? 1 : size;
    const std::size_t align = static_cast<std::size_t>(alignment);
    void* ptr = nullptr;
#if defined(_MSC_VER)
    ptr = _aligned_malloc(requested, align);
#else
    if (align == 0 || requested > std::numeric_limits<std::size_t>::max() - align) {
        throw std::bad_alloc{};
    }
    const std::size_t aligned_size = ((requested + align - 1) / align) * align;
    ptr = std::aligned_alloc(align, aligned_size);
#endif
    if (!ptr) {
        throw std::bad_alloc{};
    }
    kin::detail::record_allocation(requested);
    return ptr;
}

} // namespace

void* operator new(std::size_t size) {
    return allocate_bytes(size);
}

void* operator new[](std::size_t size) {
    return allocate_bytes(size);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return allocate_bytes(size);
    } catch (...) {
        return nullptr;
    }
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return allocate_bytes(size);
    } catch (...) {
        return nullptr;
    }
}

void* operator new(std::size_t size, std::align_val_t alignment) {
    return allocate_aligned_bytes(size, alignment);
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
    return allocate_aligned_bytes(size, alignment);
}

void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try {
        return allocate_aligned_bytes(size, alignment);
    } catch (...) {
        return nullptr;
    }
}

void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try {
        return allocate_aligned_bytes(size, alignment);
    } catch (...) {
        return nullptr;
    }
}

void operator delete(void* ptr) noexcept {
    std::free(ptr);
}

void operator delete[](void* ptr) noexcept {
    std::free(ptr);
}

void operator delete(void* ptr, std::size_t) noexcept {
    std::free(ptr);
}

void operator delete[](void* ptr, std::size_t) noexcept {
    std::free(ptr);
}

void operator delete(void* ptr, const std::nothrow_t&) noexcept {
    std::free(ptr);
}

void operator delete[](void* ptr, const std::nothrow_t&) noexcept {
    std::free(ptr);
}

void operator delete(void* ptr, std::align_val_t) noexcept {
#if defined(_MSC_VER)
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
}

void operator delete[](void* ptr, std::align_val_t) noexcept {
#if defined(_MSC_VER)
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
}

void operator delete(void* ptr, std::size_t, std::align_val_t) noexcept {
#if defined(_MSC_VER)
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
}

void operator delete[](void* ptr, std::size_t, std::align_val_t) noexcept {
#if defined(_MSC_VER)
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
}

void operator delete(void* ptr, std::align_val_t, const std::nothrow_t&) noexcept {
#if defined(_MSC_VER)
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
}

void operator delete[](void* ptr, std::align_val_t, const std::nothrow_t&) noexcept {
#if defined(_MSC_VER)
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
}
