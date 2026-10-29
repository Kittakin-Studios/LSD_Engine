// Minimal self-contained tests (no framework yet). Each CHECK failure is counted and reported.
#include "lsd/foundation/allocator.h"
#include "lsd/foundation/handle.h"
#include "lsd/foundation/hash_id.h"
#include "lsd/foundation/system_info.h"
#include "lsd/rhi/rhi.h"

#include <cstdio>
#include <cstdint>

static int g_failures = 0;
#define CHECK(expr) do { if (!(expr)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #expr); ++g_failures; } } while (0)

static void test_hash_id() {
    constexpr lsd::HashId a = LSD_ID("unit");
    static_assert(a.value == lsd::hash64("unit"), "hash must be constexpr");
    CHECK(LSD_ID("unit") == LSD_ID("unit"));
    CHECK(LSD_ID("unit") != LSD_ID("mesh"));
    CHECK(lsd::hash64("") == 0xcbf29ce484222325ull);
}

static void test_handle() {
    struct EntityTag {};
    using Entity = lsd::Handle<EntityTag>;
    const Entity e = Entity::make(1234, 7);
    CHECK(e.index() == 1234);
    CHECK(e.generation() == 7);
    CHECK(e.valid());
    CHECK(!Entity{}.valid());
    CHECK(Entity::make(1234, 8) != e);
}

static void test_heap_allocator() {
    lsd::HeapAllocator heap("test");
    void* p = heap.allocate(100, 64);
    CHECK(p != nullptr);
    CHECK(reinterpret_cast<std::uintptr_t>(p) % 64 == 0);
    CHECK(heap.allocated_bytes() == 100);
    heap.deallocate(p);
    CHECK(heap.allocated_bytes() == 0);
}

static void test_arena_allocator() {
    lsd::HeapAllocator heap("arena_backing");
    {
        lsd::ArenaAllocator arena("frame", heap, 256);
        void* a = arena.allocate(10, 16);
        void* b = arena.allocate(10, 16);
        CHECK(a && b && a != b);
        CHECK(reinterpret_cast<std::uintptr_t>(b) % 16 == 0);
        CHECK(arena.allocate(1024, 16) == nullptr);  // over capacity
        arena.reset();
        CHECK(arena.allocated_bytes() == 0);
    }
    CHECK(heap.allocated_bytes() == 0);
}

static void test_adapter_selection() {
    using namespace lsd::rhi;
    AdapterInfo adapters[2]{};
    adapters[0].type = AdapterType::Integrated;
    adapters[0].shared_memory_bytes = 16 * lsd::GiB;
    adapters[1].type = AdapterType::Discrete;
    adapters[1].dedicated_vram_bytes = 8 * lsd::GiB;
    DeviceDesc desc{};
    CHECK(select_adapter(adapters, 2, desc) == 1);   // discrete wins
    desc.adapter_override = 0;
    CHECK(select_adapter(adapters, 2, desc) == 0);   // -gpu=0 respected
    CHECK(recommend_preset(adapters[1], 32 * lsd::GiB) == QualityPreset::High);
    CHECK(recommend_preset(adapters[0], 16 * lsd::GiB) == QualityPreset::Low);
}

static void test_system_info() {
    const lsd::SystemInfo info = lsd::query_system_info();
    CHECK(info.logical_cores >= 1);
    CHECK(lsd::recommended_worker_count(info) >= 1);
}

int main() {
    test_hash_id();
    test_handle();
    test_heap_allocator();
    test_arena_allocator();
    test_adapter_selection();
    test_system_info();
    if (g_failures == 0) std::printf("All foundation tests passed.\n");
    return g_failures == 0 ? 0 : 1;
}
