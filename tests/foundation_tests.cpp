// Minimal self-contained tests (no framework yet). Each CHECK failure is counted and reported.
#include "lsd/core/console.h"
#include "lsd/core/job_system.h"
#include "lsd/foundation/log.h"
#include "lsd/foundation/allocator.h"
#include "lsd/foundation/handle.h"
#include "lsd/foundation/hash_id.h"
#include "lsd/foundation/math.h"
#include "lsd/foundation/system_info.h"
#include "lsd/rhi/rhi.h"

#include <atomic>
#include <cstdio>
#include <cstdint>
#include <cstring>

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
    CHECK(heap.allocated_bytes() == 0 && heap.peak_bytes() == 100 && heap.live_allocations() == 0);
    lsd::AllocatorStats stats[64];
    const lsd::u32 n = lsd::allocator_stats(stats, 64);
    bool found = false;
    for (lsd::u32 i = 0; i < n; ++i) found = found || (std::strcmp(stats[i].name, "test") == 0 && stats[i].peak == 100 && !stats[i].arena);
    CHECK(found);  // listed for the memory report
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

static void test_job_system() {
    lsd::HeapAllocator heap("jobs");
    for (lsd::u32 workers : {0u, 4u}) {
        lsd::JobSystem jobs(heap, workers, 64);  // small queue so the full-queue path runs too
        std::atomic<int> sum{0};
        lsd::JobCounter counter;
        for (int i = 1; i <= 1000; ++i) {
            jobs.run([](void* data) { static_cast<std::atomic<int>*>(data)->fetch_add(1); }, &sum, &counter);
        }
        jobs.wait(counter);
        CHECK(sum.load() == 1000);
        CHECK(counter.pending.load() == 0);
    }
    CHECK(heap.allocated_bytes() == 0);
}

static int g_sink_lines = 0;
static void count_sink(void*, lsd::log::Level, const char*, const char*) { ++g_sink_lines; }

static void test_console() {
    namespace console = lsd::console;
    console::remove_all();
    bool grid = true;
    lsd::i32 count = 3;
    lsd::f32 speed = 1.0f;
    int called = 0;
    char got[64] = "";
    struct Ctx { int* called; char* got; } ctx{&called, got};
    console::add_variable("r.Grid", &grid, "Draw the grid");
    console::add_variable("r.Count", &count, "Count");
    console::add_variable("cam.Speed", &speed, "Speed");
    console::add_command("stat", [](void* user, const char* args) {
        auto* c = static_cast<Ctx*>(user);
        ++*c->called;
        std::snprintf(c->got, 64, "%s", args);
    }, &ctx, "Toggle a stat");
    lsd::log::set_sink(count_sink, nullptr);
    CHECK(console::execute("r.grid 0") && !grid);  // names are case-insensitive
    CHECK(console::execute("  r.Count   -7  ") && count == -7);
    CHECK(console::execute("cam.Speed 2.5") && speed == 2.5f);
    CHECK(!console::execute("cam.Speed fast") && speed == 2.5f);
    CHECK(!console::execute("r.Grid maybe") && !grid);
    CHECK(console::execute("stat fps ") && called == 1 && std::strcmp(got, "fps") == 0);
    CHECK(!console::execute("nope 1"));
    CHECK(console::execute("r.Grid"));  // prints the value
    CHECK(g_sink_lines >= 5);
    lsd::log::set_sink(nullptr, nullptr);
    const char* names[8];
    CHECK(console::complete("r.", names, 8) == 2);
    CHECK(console::help("STAT") && std::strcmp(console::help("stat"), "Toggle a stat") == 0);
    console::remove_all();
}

using lsd::Mat4;
using lsd::Vec3;
using lsd::f32;
using lsd::perspective;
using lsd::look_at;
using lsd::inverse;

static void test_inverse() {  // TAA reprojects with it
    const Mat4 m = perspective(1.0f, 1.6f, 0.05f, 1000.0f) * look_at(Vec3{3, -2, 1.5f}, Vec3{0, 0, 0}, Vec3{0, 0, 1});
    const Mat4 i = m * inverse(m);
    f32 worst = 0.0f;
    for (int k = 0; k < 16; ++k) {
        const f32 expected = (k % 5 == 0) ? 1.0f : 0.0f;
        const f32 e = i.m[k] > expected ? i.m[k] - expected : expected - i.m[k];
        worst = e > worst ? e : worst;
    }
    CHECK(worst < 1e-4f);
}

// Memory per system: named sources next to the allocators; reallocate keeps contents and the count.
static lsd::usize g_source_bytes = 1234;
static void test_memory_sources() {
    using namespace lsd;
    add_memory_source("Test Source", [](void*) { return g_source_bytes; }, &g_source_bytes);
    AllocatorStats stats[128];
    auto find = [&](const char* name) -> const AllocatorStats* {
        const u32 n = allocator_stats(stats, 128);
        for (u32 i = 0; i < n; ++i) {
            if (std::strcmp(stats[i].name, name) == 0) return &stats[i];
        }
        return nullptr;
    };
    const AllocatorStats* s = find("Test Source");
    CHECK(s && s->bytes == 1234 && s->peak == 1234);
    g_source_bytes = 10;
    s = find("Test Source");
    CHECK(s && s->bytes == 10 && s->peak == 1234);  // the peak stays
    remove_memory_source(&g_source_bytes);
    CHECK(!find("Test Source"));
    HeapAllocator heap("Test Realloc");
    char* p = static_cast<char*>(heap.allocate(8, 16));
    std::memcpy(p, "abcdefg", 8);
    p = static_cast<char*>(heap.reallocate(p, 64, 16));
    CHECK(p && std::strcmp(p, "abcdefg") == 0 && HeapAllocator::block_size(p) == 64 && heap.allocated_bytes() == 64 && heap.live_allocations() == 1);
    CHECK(heap.reallocate(p, 0, 16) == nullptr && heap.allocated_bytes() == 0 && heap.live_allocations() == 0);
}

int main() {
    test_console();
    test_memory_sources();
    test_inverse();
    test_hash_id();
    test_handle();
    test_heap_allocator();
    test_arena_allocator();
    test_adapter_selection();
    test_system_info();
    test_job_system();
    if (g_failures == 0) std::printf("All foundation tests passed.\n");
    return g_failures == 0 ? 0 : 1;
}
