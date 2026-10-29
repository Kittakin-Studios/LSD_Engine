#include "lsd/foundation/allocator.h"
#include "lsd/foundation/assert.h"
#include "lsd/foundation/log.h"

#include <cstdlib>
#include <cstring>

namespace lsd {

namespace {
// Each heap block stores a small header just before the returned pointer so that
// deallocate() can recover the original block and its size for tracking.
struct BlockHeader {
    void* original;
    usize size;
};
} // namespace

HeapAllocator::~HeapAllocator() {
    const usize live = live_allocations_.load();
    if (live != 0) {
        LSD_LOG_WARN("memory", "Allocator '%s' destroyed with %zu live allocations (%zu bytes)",
                     name_, live, allocated_.load());
    }
}

void* HeapAllocator::allocate(usize size, usize alignment) {
    LSD_ASSERT((alignment & (alignment - 1)) == 0);
    const usize total = size + alignment + sizeof(BlockHeader);
    void* original = std::malloc(total);
    if (!original) return nullptr;
    const auto base = reinterpret_cast<usize>(original) + sizeof(BlockHeader);
    const usize aligned = align_up(base, alignment);
    auto* header = reinterpret_cast<BlockHeader*>(aligned - sizeof(BlockHeader));
    header->original = original;
    header->size = size;
    allocated_.fetch_add(size, std::memory_order_relaxed);
    live_allocations_.fetch_add(1, std::memory_order_relaxed);
    return reinterpret_cast<void*>(aligned);
}

void HeapAllocator::deallocate(void* ptr) {
    if (!ptr) return;
    auto* header = reinterpret_cast<BlockHeader*>(reinterpret_cast<usize>(ptr) - sizeof(BlockHeader));
    allocated_.fetch_sub(header->size, std::memory_order_relaxed);
    live_allocations_.fetch_sub(1, std::memory_order_relaxed);
    std::free(header->original);
}

ArenaAllocator::ArenaAllocator(const char* name, Allocator& backing, usize capacity)
    : name_(name), backing_(backing), capacity_(capacity) {
    memory_ = static_cast<u8*>(backing_.allocate(capacity, 64));
    LSD_ASSERT(memory_ != nullptr);
}

ArenaAllocator::~ArenaAllocator() { backing_.deallocate(memory_); }

void* ArenaAllocator::allocate(usize size, usize alignment) {
    const usize start = align_up(offset_, alignment);
    if (start + size > capacity_) {
        LSD_LOG_ERROR("memory", "Arena '%s' out of memory (%zu / %zu bytes)", name_, start + size, capacity_);
        return nullptr;
    }
    offset_ = start + size;
    return memory_ + start;
}

Allocator& default_allocator() {
    static HeapAllocator heap("default");
    return heap;
}

} // namespace lsd
