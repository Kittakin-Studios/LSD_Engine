#include "lsd/foundation/allocator.h"
#include "lsd/foundation/assert.h"
#include "lsd/foundation/log.h"

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

namespace lsd {

namespace {
// Each heap block stores a small header just before the returned pointer so that
// deallocate() can recover the original block and its size for tracking.
struct BlockHeader {
    void* original;
    usize size;
};

// The live allocators, for allocator_stats. A function-local static: allocators made during static
// initialisation (default_allocator) register safely.
struct Source {
    const char* name;
    MemorySource fn;
    void* user;
    usize peak;
};
struct Registry {
    std::mutex mutex;
    std::vector<Allocator*> list;
    std::vector<Source> sources;
};
Registry& registry() {
    static Registry r;
    return r;
}
void add(Allocator* a) {
    std::lock_guard<std::mutex> lock(registry().mutex);
    registry().list.push_back(a);
}
void remove(Allocator* a) {
    std::lock_guard<std::mutex> lock(registry().mutex);
    auto& list = registry().list;
    for (usize i = 0; i < list.size(); ++i) {
        if (list[i] == a) {
            list.erase(list.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    }
}
} // namespace

u32 allocator_stats(AllocatorStats* out, u32 capacity) {
    std::lock_guard<std::mutex> lock(registry().mutex);
    u32 n = 0;
    for (Allocator* a : registry().list) {
        if (n == capacity) break;
        AllocatorStats& s = out[n++];
        s.name = a->name();
        s.bytes = a->allocated_bytes();
        if (auto* heap = dynamic_cast<HeapAllocator*>(a)) {
            s.peak = heap->peak_bytes();
            s.allocations = heap->live_allocations();
            s.arena = false;
        } else {
            auto* arena = dynamic_cast<ArenaAllocator*>(a);
            s.peak = arena ? arena->capacity() : s.bytes;
            s.allocations = 0;
            s.arena = arena != nullptr;
        }
    }
    for (Source& src : registry().sources) {
        if (n == capacity) break;
        AllocatorStats& s = out[n++];
        s.name = src.name;
        s.bytes = src.fn(src.user);
        src.peak = s.bytes > src.peak ? s.bytes : src.peak;
        s.peak = src.peak;
        s.allocations = 0;
        s.arena = false;
    }
    return n;
}

void add_memory_source(const char* name, MemorySource fn, void* user) {
    std::lock_guard<std::mutex> lock(registry().mutex);
    registry().sources.push_back({name, fn, user, 0});
}

void remove_memory_source(void* user) {
    std::lock_guard<std::mutex> lock(registry().mutex);
    auto& list = registry().sources;
    for (usize i = list.size(); i-- > 0;) {
        if (list[i].user == user) list.erase(list.begin() + static_cast<std::ptrdiff_t>(i));
    }
}

usize HeapAllocator::block_size(const void* ptr) {
    return ptr ? reinterpret_cast<const BlockHeader*>(reinterpret_cast<usize>(ptr) - sizeof(BlockHeader))->size : 0;
}

void* HeapAllocator::reallocate(void* ptr, usize size, usize alignment) {
    void* fresh = size ? allocate(size, alignment) : nullptr;
    if (ptr && fresh) {
        const usize old = block_size(ptr);
        std::memcpy(fresh, ptr, old < size ? old : size);
    }
    if (ptr && (fresh || !size)) deallocate(ptr);
    return fresh;
}

HeapAllocator::HeapAllocator(const char* name) : name_(name) { add(this); }

HeapAllocator::~HeapAllocator() {
    remove(this);
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
    const usize now = allocated_.fetch_add(size, std::memory_order_relaxed) + size;
    usize peak = peak_.load(std::memory_order_relaxed);
    while (now > peak && !peak_.compare_exchange_weak(peak, now, std::memory_order_relaxed)) {}
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
    add(this);
}

ArenaAllocator::~ArenaAllocator() {
    remove(this);
    backing_.deallocate(memory_);
}

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
