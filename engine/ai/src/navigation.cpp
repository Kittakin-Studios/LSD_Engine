#include "lsd/ai/navigation.h"

#include "lsd/foundation/log.h"

#include <DetourCrowd.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <DetourNavMeshQuery.h>
#include <Recast.h>

#include <cmath>
#include <cstring>
#include <vector>

namespace lsd::ai {

namespace {

// Ours: Z up. Recast: Y up. (x, y, z) -> (x, z, -y) is a rotation, so winding is kept.
void to_recast(Vec3 v, f32* out) {
    out[0] = v.x;
    out[1] = v.z;
    out[2] = -v.y;
}
Vec3 from_recast(const f32* v) { return {v[0], -v[2], v[1]}; }

constexpr f32 kSearchExtents[3] = {2.0f, 4.0f, 2.0f};  // how far from the mesh a point may be (x, up, z)
constexpr int kMaxPathPolys = 256;

} // namespace

struct NavMesh::Impl {
    dtNavMesh* mesh = nullptr;
    dtNavMeshQuery* query = nullptr;
    dtCrowd* crowd = nullptr;
    dtQueryFilter filter;

    void release() {
        if (crowd) dtFreeCrowd(crowd);
        if (query) dtFreeNavMeshQuery(query);
        if (mesh) dtFreeNavMesh(mesh);
        crowd = nullptr;
        query = nullptr;
        mesh = nullptr;
    }
};

NavMesh::NavMesh() : impl_(new Impl()) { impl_->filter.setIncludeFlags(0xFFFF); }

NavMesh::~NavMesh() {
    impl_->release();
    delete impl_;
}

bool NavMesh::valid() const { return impl_->query != nullptr; }
void NavMesh::clear() { impl_->release(); }

bool NavMesh::build(const f32* vertices, u32 vertex_count, const u32* indices, u32 triangle_count, Vec3 bounds_min, Vec3 bounds_max,
                    const NavSettings& s, const NavCutout* cutouts, u32 cutout_count) {
    impl_->release();
    if (!vertex_count || !triangle_count) return false;
    // shortcut: one tile, built in one go (Recast's "solo mesh"); tile the build for big worlds.
    std::vector<f32> verts(vertex_count * 3);
    for (u32 i = 0; i < vertex_count; ++i) to_recast({vertices[i * 3], vertices[i * 3 + 1], vertices[i * 3 + 2]}, &verts[i * 3]);
    std::vector<int> tris(indices, indices + triangle_count * 3);
    f32 a[3], b[3];
    to_recast(bounds_min, a);
    to_recast(bounds_max, b);
    rcConfig cfg{};
    cfg.cs = s.cell_size;
    cfg.ch = s.cell_height;
    for (int k = 0; k < 3; ++k) {
        cfg.bmin[k] = std::fmin(a[k], b[k]);
        cfg.bmax[k] = std::fmax(a[k], b[k]);
    }
    cfg.walkableSlopeAngle = s.max_slope;
    cfg.walkableHeight = static_cast<int>(std::ceil(s.agent_height / cfg.ch));
    cfg.walkableClimb = static_cast<int>(std::floor(s.max_climb / cfg.ch));
    cfg.walkableRadius = static_cast<int>(std::ceil(s.agent_radius / cfg.cs));
    cfg.maxEdgeLen = static_cast<int>(12.0f / cfg.cs);
    cfg.maxSimplificationError = 1.3f;
    cfg.minRegionArea = 8 * 8;
    cfg.mergeRegionArea = 20 * 20;
    cfg.maxVertsPerPoly = 6;
    cfg.detailSampleDist = cfg.cs * 6.0f;
    cfg.detailSampleMaxError = cfg.ch;
    rcCalcGridSize(cfg.bmin, cfg.bmax, cfg.cs, &cfg.width, &cfg.height);
    if (static_cast<i64>(cfg.width) * cfg.height > 4096LL * 4096LL) {
        LSD_LOG_WARN("ai", "Navmesh area too large for one tile (%d x %d cells): raise the cell size or add Nav Mesh Bounds", cfg.width, cfg.height);
        return false;
    }

    rcContext ctx(false);
    rcHeightfield* solid = rcAllocHeightfield();
    rcCompactHeightfield* chf = rcAllocCompactHeightfield();
    rcContourSet* contours = rcAllocContourSet();
    rcPolyMesh* poly = rcAllocPolyMesh();
    rcPolyMeshDetail* detail = rcAllocPolyMeshDetail();
    std::vector<unsigned char> areas(triangle_count, 0);
    bool ok = rcCreateHeightfield(&ctx, *solid, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch);
    if (ok) {
        rcMarkWalkableTriangles(&ctx, cfg.walkableSlopeAngle, verts.data(), static_cast<int>(vertex_count), tris.data(), static_cast<int>(triangle_count), areas.data());
        ok = rcRasterizeTriangles(&ctx, verts.data(), static_cast<int>(vertex_count), tris.data(), areas.data(), static_cast<int>(triangle_count), *solid, cfg.walkableClimb);
    }
    if (ok) {
        rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *solid);
        rcFilterLedgeSpans(&ctx, cfg.walkableHeight, cfg.walkableClimb, *solid);
        rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *solid);
        ok = rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb, *solid, *chf) && rcErodeWalkableArea(&ctx, cfg.walkableRadius, *chf);
        for (u32 i = 0; ok && i < cutout_count; ++i) {  // Recast's height axis is our Z
            f32 footprint[12];
            for (int c = 0; c < 4; ++c) to_recast(cutouts[i].corners[c], &footprint[c * 3]);
            rcMarkConvexPolyArea(&ctx, footprint, 4, cutouts[i].z_min, cutouts[i].z_max, RC_NULL_AREA, *chf);
        }
        ok = ok && rcBuildDistanceField(&ctx, *chf) && rcBuildRegions(&ctx, *chf, 0, cfg.minRegionArea, cfg.mergeRegionArea) &&
             rcBuildContours(&ctx, *chf, cfg.maxSimplificationError, cfg.maxEdgeLen, *contours) && rcBuildPolyMesh(&ctx, *contours, cfg.maxVertsPerPoly, *poly) &&
             rcBuildPolyMeshDetail(&ctx, *poly, *chf, cfg.detailSampleDist, cfg.detailSampleMaxError, *detail);
    }
    unsigned char* data = nullptr;
    int data_size = 0;
    if (ok && poly->npolys > 0) {
        for (int i = 0; i < poly->npolys; ++i) poly->flags[i] = poly->areas[i] == RC_WALKABLE_AREA ? 1 : 0;
        dtNavMeshCreateParams params{};
        params.verts = poly->verts;
        params.vertCount = poly->nverts;
        params.polys = poly->polys;
        params.polyAreas = poly->areas;
        params.polyFlags = poly->flags;
        params.polyCount = poly->npolys;
        params.nvp = poly->nvp;
        params.detailMeshes = detail->meshes;
        params.detailVerts = detail->verts;
        params.detailVertsCount = detail->nverts;
        params.detailTris = detail->tris;
        params.detailTriCount = detail->ntris;
        params.walkableHeight = s.agent_height;
        params.walkableRadius = s.agent_radius;
        params.walkableClimb = s.max_climb;
        rcVcopy(params.bmin, poly->bmin);
        rcVcopy(params.bmax, poly->bmax);
        params.cs = cfg.cs;
        params.ch = cfg.ch;
        params.buildBvTree = true;
        ok = dtCreateNavMeshData(&params, &data, &data_size);
    } else {
        ok = false;
    }
    rcFreeHeightField(solid);
    rcFreeCompactHeightfield(chf);
    rcFreeContourSet(contours);
    rcFreePolyMesh(poly);
    rcFreePolyMeshDetail(detail);
    if (!ok) return false;

    impl_->mesh = dtAllocNavMesh();
    if (dtStatusFailed(impl_->mesh->init(data, data_size, DT_TILE_FREE_DATA))) {
        dtFree(data);
        impl_->release();
        return false;
    }
    impl_->query = dtAllocNavMeshQuery();
    impl_->crowd = dtAllocCrowd();
    if (dtStatusFailed(impl_->query->init(impl_->mesh, 2048)) || !impl_->crowd->init(static_cast<int>(kMaxAgents), 2.0f, impl_->mesh)) {
        impl_->release();
        return false;
    }
    return true;
}

bool NavMesh::nearest_point(Vec3 point, Vec3& out) const {
    if (!impl_->query) return false;
    f32 p[3], nearest[3];
    to_recast(point, p);
    dtPolyRef ref = 0;
    if (dtStatusFailed(impl_->query->findNearestPoly(p, kSearchExtents, &impl_->filter, &ref, nearest)) || !ref) return false;
    out = from_recast(nearest);
    return true;
}

bool NavMesh::random_point(Vec3 around, f32 radius, Vec3& out) const {
    if (!impl_->query) return false;
    f32 p[3], start[3], point[3];
    to_recast(around, p);
    dtPolyRef start_ref = 0, ref = 0;
    impl_->query->findNearestPoly(p, kSearchExtents, &impl_->filter, &start_ref, start);
    if (!start_ref) return false;
    static u32 seed = 0x2545F491u;
    auto frand = []() {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        return static_cast<f32>(seed >> 8) * (1.0f / 16777216.0f);
    };
    if (dtStatusFailed(impl_->query->findRandomPointAroundCircle(start_ref, start, radius, &impl_->filter, frand, &ref, point)) || !ref) return false;
    out = from_recast(point);
    return true;
}

u32 NavMesh::find_path(Vec3 from, Vec3 to, Vec3* out, u32 capacity) const {
    if (!impl_->query || capacity < 2) return 0;
    f32 a[3], b[3], start[3], end[3];
    to_recast(from, a);
    to_recast(to, b);
    dtPolyRef start_ref = 0, end_ref = 0;
    impl_->query->findNearestPoly(a, kSearchExtents, &impl_->filter, &start_ref, start);
    impl_->query->findNearestPoly(b, kSearchExtents, &impl_->filter, &end_ref, end);
    if (!start_ref || !end_ref) return 0;
    dtPolyRef polys[kMaxPathPolys];
    int poly_count = 0;
    impl_->query->findPath(start_ref, end_ref, start, end, &impl_->filter, polys, &poly_count, kMaxPathPolys);
    if (!poly_count) return 0;
    if (polys[poly_count - 1] != end_ref) impl_->query->closestPointOnPoly(polys[poly_count - 1], end, end, nullptr);  // partial: as close as it gets
    std::vector<f32> straight(capacity * 3);
    int count = 0;
    impl_->query->findStraightPath(start, end, polys, poly_count, straight.data(), nullptr, nullptr, &count, static_cast<int>(capacity));
    for (int i = 0; i < count; ++i) out[i] = from_recast(&straight[i * 3]);
    return static_cast<u32>(count);
}

u32 NavMesh::debug_lines(Vec3* out, u32 capacity) const {
    if (!impl_->mesh) return 0;
    u32 n = 0;
    const dtNavMesh* mesh = impl_->mesh;
    for (int t = 0; t < mesh->getMaxTiles(); ++t) {
        const dtMeshTile* tile = mesh->getTile(t);
        if (!tile || !tile->header) continue;
        for (int p = 0; p < tile->header->polyCount; ++p) {
            const dtPoly& poly = tile->polys[p];
            if (poly.getType() == DT_POLYTYPE_OFFMESH_CONNECTION) continue;
            for (int k = 0; k < poly.vertCount && n + 2 <= capacity; ++k) {
                const Vec3 lift{0, 0, 0.05f};  // just above the floor
                out[n++] = from_recast(&tile->verts[poly.verts[k] * 3]) + lift;
                out[n++] = from_recast(&tile->verts[poly.verts[(k + 1) % poly.vertCount] * 3]) + lift;
            }
        }
    }
    return n;
}

i32 NavMesh::add_agent(Vec3 position, f32 radius, f32 height, f32 max_speed) {
    if (!impl_->crowd) return -1;
    Vec3 on_mesh;
    if (!nearest_point(position, on_mesh)) return -1;
    dtCrowdAgentParams ap{};
    ap.radius = radius;
    ap.height = height;
    ap.maxSpeed = max_speed;
    ap.maxAcceleration = max_speed * 4.0f;
    ap.collisionQueryRange = radius * 12.0f;
    ap.pathOptimizationRange = radius * 30.0f;
    ap.updateFlags = DT_CROWD_ANTICIPATE_TURNS | DT_CROWD_OPTIMIZE_VIS | DT_CROWD_OPTIMIZE_TOPO | DT_CROWD_OBSTACLE_AVOIDANCE | DT_CROWD_SEPARATION;
    ap.obstacleAvoidanceType = 3;
    ap.separationWeight = 2.0f;
    f32 p[3];
    to_recast(on_mesh, p);
    return impl_->crowd->addAgent(p, &ap);
}

void NavMesh::remove_agent(i32 agent) {
    if (impl_->crowd && agent >= 0) impl_->crowd->removeAgent(agent);
}

bool NavMesh::move_agent(i32 agent, Vec3 target) {
    if (!impl_->crowd || agent < 0) return false;
    f32 t[3], nearest[3];
    to_recast(target, t);
    dtPolyRef ref = 0;
    impl_->query->findNearestPoly(t, kSearchExtents, &impl_->filter, &ref, nearest);
    return ref && impl_->crowd->requestMoveTarget(agent, ref, nearest);
}

void NavMesh::stop_agent(i32 agent) {
    if (impl_->crowd && agent >= 0) impl_->crowd->resetMoveTarget(agent);
}

void NavMesh::update(f32 dt) {
    if (impl_->crowd && dt > 0.0f) impl_->crowd->update(dt, nullptr);
}

Vec3 NavMesh::agent_position(i32 agent) const {
    const dtCrowdAgent* a = impl_->crowd && agent >= 0 ? impl_->crowd->getAgent(agent) : nullptr;
    return a && a->active ? from_recast(a->npos) : Vec3{};
}

Vec3 NavMesh::agent_velocity(i32 agent) const {
    const dtCrowdAgent* a = impl_->crowd && agent >= 0 ? impl_->crowd->getAgent(agent) : nullptr;
    return a && a->active ? from_recast(a->vel) : Vec3{};
}

bool NavMesh::agent_moving(i32 agent) const {
    const dtCrowdAgent* a = impl_->crowd && agent >= 0 ? impl_->crowd->getAgent(agent) : nullptr;
    if (!a || !a->active || a->targetState == DT_CROWDAGENT_TARGET_NONE || a->targetState == DT_CROWDAGENT_TARGET_FAILED) return false;
    const f32 dx = a->npos[0] - a->targetPos[0], dz = a->npos[2] - a->targetPos[2];
    return dx * dx + dz * dz > a->params.radius * a->params.radius * 0.25f;
}

} // namespace lsd::ai
