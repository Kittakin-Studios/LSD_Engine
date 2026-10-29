#pragma once
// Every system in LSD_Engine receives an Allocator instead of calling new/delete directly.
// This makes memory use visible per system and lets hot paths use cheap arena allocators.
#include "lsd/foundation/types.h"
#include <atomic>

namespace lsd {

class Allocator {
public:
    virtual ~Allocator() = default;
    virtual void* allocate(usize size, usize alignment = alignof(std::max_align_t)) = 0;
    virtual void  deallocate(void* ptr) = 0;
    // Bytes currently allocated through this allocator (best effort).
    virtual usize allocated_bytes() const = 0;
    virtual const char* name() const = 0;
};

// General-purpose allocator backed by the system heap, with per-instance tracking.
class HeapAllocator final : public Allocator {
public:
    explicit HeapAllocator(const char* name) : name_(name) {}
    ~HeapAllocator() override;
    void* allocate(usize size, usize alignment) override;
    void  deallocate(void* ptr) override;
    usize allocated_bytes() const override { return allocated_.load(std::memory_order_relaxed); }
    const char* name() const override { return name_; }
private:
    const char* name_;
    std::atomic<usize> allocated_{0};
    std::atomic<usize> live_allocations_{0};
};

// Linear "bump" allocator for per-frame or scoped scratch memory.
// Individual frees are no-ops; call reset() to release everything at once.
class ArenaAllocator final : public Allocator {
public:
    ArenaAllocator(const char* name, Allocator& backing, usize capacity);
    ~ArenaAllocator() override;
    void* allocate(usize size, usize alignment) override;
    void  deallocate(void*) override {}
    usize allocated_bytes() const override { return offset_; }
    const char* name() const override { return name_; }
    void  reset() { offset_ = 0; }
    usize capacity() const { return capacity_; }
private:
    const char* name_;
    Allocator& backing_;
    u8* memory_ = nullptr;
    usize capacity_ = 0;
    usize offset_ = 0;
};

// Process-wide default heap allocator.
Allocator& default_allocator();

} // namespace lsd
