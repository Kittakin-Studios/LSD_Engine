#pragma once
// Environment queries (Unreal's EQS): generate candidate points (rings or a grid around the querier or a
// target), test each (distance, line of sight to the target, reachable, facing), drop the ones a filter
// rejects, score the rest and return the best (cover spots, flanking points, a place to stand and shoot).
#include "lsd/foundation/math.h"

namespace lsd::ai {

enum class QueryGenerator : u8 { Rings, Grid };
enum class QueryTestType : u8 { Distance, Visible, Reachable, Facing };

struct QueryTest {
    QueryTestType type = QueryTestType::Distance;
    bool to_target = true;   // Distance: from the target (else from the querier)
    bool filter = false;     // drop points outside [min, max] (Visible / Reachable: the ones that fail)
    bool score = true;       // adds to the score
    f32 min = 0.0f, max = 1e9f;
    f32 weight = 1.0f;       // > 0 prefers higher values (far, visible, ahead), < 0 lower (near, hidden, behind)
};

constexpr u32 kMaxQueryTests = 4;
constexpr u32 kMaxQueryPoints = 256;

struct EnvQuery {
    QueryGenerator generator = QueryGenerator::Rings;
    bool around_target = false;  // centred on the target (else on the querier)
    f32 radius = 8.0f;           // Rings: the outer ring; Grid: half its width
    f32 spacing = 2.0f;          // Grid: between points
    u32 rings = 2;
    u32 points_per_ring = 12;
    bool on_navmesh = true;      // points snap onto the navmesh; ones off it are dropped
    QueryTest tests[kMaxQueryTests];
    u32 test_count = 0;
};

// What the game answers for the tests.
struct QueryHost {
    void* user = nullptr;
    bool (*project)(void* user, Vec3 point, Vec3& out) = nullptr;   // the nearest navmesh point
    bool (*visible)(void* user, Vec3 from, Vec3 to) = nullptr;      // nothing solid between (eye height added by the caller)
    bool (*reachable)(void* user, Vec3 from, Vec3 to) = nullptr;    // a navmesh path joins them
};

struct QueryPoint {
    Vec3 at;
    f32 score = 0.0f;  // 0..sum of |weights| after scoring
    bool kept = true;  // false: a filter dropped it
};

// Fills `out` (count = points made) and returns the best point's index, -1 if every point was dropped.
// `forward` is the querier's facing (Facing test: 1 straight ahead, -1 behind).
i32 run_query(const EnvQuery& query, const QueryHost& host, Vec3 querier, Vec3 forward, Vec3 target, QueryPoint* out, u32 capacity, u32& count);

} // namespace lsd::ai
