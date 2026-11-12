#include "lsd/ai/navigation.h"

#include "lsd/foundation/log.h"

#include <DetourCrowd.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <DetourNavMeshQuery.h>
#include <Recast.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <thread>
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
    // The tiled build's layout and each tile's input hash (what a rebuild compares).
    int tw = 0, th = 0;
    f32 bmin[3] = {};
    NavSettings settings;
    std::vector<u64> hashes;
    int max_tiles = 0;  // what the Detour mesh was made for
    u32 rebuilt_tiles = 0;

    void release() {
        if (crowd) dtFreeCrowd(crowd);
        if (query) dtFreeNavMeshQuery(query);
        if (mesh) dtFreeNavMesh(mesh);
        crowd = nullptr;
        query = nullptr;
        mesh = nullptr;
        hashes.clear();
        tw = th = 0;
    }
};

NavMesh::NavMesh() : impl_(new Impl()) { impl_->filter.setIncludeFlags(0xFFFF); }

NavMesh::~NavMesh() {
    impl_->release();
    delete impl_;
}

bool NavMesh::valid() const { return impl_->query != nullptr; }
void NavMesh::clear() { impl_->release(); }

namespace {

constexpr int kTileCells = 64;  // cells across a tile (Recast's tiled sample): a change rebuilds only the tiles it touches

u64 mix(u64 h, const void* p, usize n) {
    for (usize i = 0; i < n; ++i) h = (h ^ static_cast<const u8*>(p)[i]) * 1099511628211ull;
    return h;
}

// One tile's Detour data from the triangles overlapping it (Recast coordinates); null when nothing in it is walkable.
bool build_tile(const rcConfig& base, const f32* bmin, int tx, int ty, const std::vector<f32>& verts, const std::vector<int>& tris,
                const std::vector<int>& tile_tris, const NavSettings& s, const NavCutout* cutouts, u32 cutout_count, const NavLink* links,
                u32 link_count, unsigned char** data, int* data_size) {
    *data = nullptr;
    *data_size = 0;
    rcConfig cfg = base;
    cfg.tileSize = kTileCells;
    cfg.borderSize = cfg.walkableRadius + 3;  // neighbouring tiles' edges line up
    cfg.width = cfg.height = kTileCells + cfg.borderSize * 2;
    const f32 tile = kTileCells * cfg.cs;
    cfg.bmin[0] = bmin[0] + static_cast<f32>(tx) * tile - static_cast<f32>(cfg.borderSize) * cfg.cs;
    cfg.bmin[2] = bmin[2] + static_cast<f32>(ty) * tile - static_cast<f32>(cfg.borderSize) * cfg.cs;
    cfg.bmax[0] = bmin[0] + static_cast<f32>(tx + 1) * tile + static_cast<f32>(cfg.borderSize) * cfg.cs;
    cfg.bmax[2] = bmin[2] + static_cast<f32>(ty + 1) * tile + static_cast<f32>(cfg.borderSize) * cfg.cs;
    if (tile_tris.empty()) return true;
    std::vector<int> local(tile_tris.size() * 3);
    for (usize i = 0; i < tile_tris.size(); ++i) {
        for (int k = 0; k < 3; ++k) local[i * 3 + static_cast<usize>(k)] = tris[static_cast<usize>(tile_tris[i]) * 3 + static_cast<usize>(k)];
    }
    const int nverts = static_cast<int>(verts.size() / 3), ntris = static_cast<int>(tile_tris.size());
    rcContext ctx(false);
    rcHeightfield* solid = rcAllocHeightfield();
    rcCompactHeightfield* chf = rcAllocCompactHeightfield();
    rcContourSet* contours = rcAllocContourSet();
    rcPolyMesh* poly = rcAllocPolyMesh();
    rcPolyMeshDetail* detail = rcAllocPolyMeshDetail();
    std::vector<unsigned char> areas(static_cast<usize>(ntris), 0);
    bool ok = rcCreateHeightfield(&ctx, *solid, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch);
    if (ok) {
        rcMarkWalkableTriangles(&ctx, cfg.walkableSlopeAngle, verts.data(), nverts, local.data(), ntris, areas.data());
        ok = rcRasterizeTriangles(&ctx, verts.data(), nverts, local.data(), areas.data(), ntris, *solid, cfg.walkableClimb);
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
        ok = ok && rcBuildDistanceField(&ctx, *chf) && rcBuildRegions(&ctx, *chf, cfg.borderSize, cfg.minRegionArea, cfg.mergeRegionArea) &&
             rcBuildContours(&ctx, *chf, cfg.maxSimplificationError, cfg.maxEdgeLen, *contours) && rcBuildPolyMesh(&ctx, *contours, cfg.maxVertsPerPoly, *poly) &&
             rcBuildPolyMeshDetail(&ctx, *poly, *chf, cfg.detailSampleDist, cfg.detailSampleMaxError, *detail);
    }
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
        params.tileX = tx;
        params.tileY = ty;
        params.tileLayer = 0;
        rcVcopy(params.bmin, poly->bmin);
        rcVcopy(params.bmax, poly->bmax);
        params.cs = cfg.cs;
        params.ch = cfg.ch;
        params.buildBvTree = true;
        // Nav links as Detour off-mesh connections, in the tile their start is in: walkable area, flag 1.
        std::vector<f32> link_verts;
        std::vector<f32> link_radius;
        std::vector<unsigned char> link_dir, link_area;
        std::vector<unsigned short> link_flags;
        std::vector<unsigned int> link_ids;
        for (u32 i = 0; i < link_count; ++i) {
            f32 a[3], b[3];
            to_recast(links[i].start, a);
            to_recast(links[i].end, b);
            const f32 x0 = bmin[0] + static_cast<f32>(tx) * tile, z0 = bmin[2] + static_cast<f32>(ty) * tile;
            if (a[0] < x0 || a[0] >= x0 + tile || a[2] < z0 || a[2] >= z0 + tile) continue;
            link_verts.insert(link_verts.end(), {a[0], a[1], a[2], b[0], b[1], b[2]});
            link_radius.push_back(links[i].radius);
            link_dir.push_back(links[i].both_ways ? DT_OFFMESH_CON_BIDIR : 0);
            link_area.push_back(RC_WALKABLE_AREA);
            link_flags.push_back(1);
            link_ids.push_back(i);
        }
        if (!link_radius.empty()) {
            params.offMeshConVerts = link_verts.data();
            params.offMeshConRad = link_radius.data();
            params.offMeshConDir = link_dir.data();
            params.offMeshConAreas = link_area.data();
            params.offMeshConFlags = link_flags.data();
            params.offMeshConUserID = link_ids.data();
            params.offMeshConCount = static_cast<int>(link_radius.size());
        }
        ok = dtCreateNavMeshData(&params, data, data_size);
    }
    rcFreeHeightField(solid);
    rcFreeCompactHeightfield(chf);
    rcFreeContourSet(contours);
    rcFreePolyMesh(poly);
    rcFreePolyMeshDetail(detail);
    return ok;
}

} // namespace

bool NavMesh::build(const f32* vertices, u32 vertex_count, const u32* indices, u32 triangle_count, Vec3 bounds_min, Vec3 bounds_max,
                    const NavSettings& s, const NavCutout* cutouts, u32 cutout_count, const NavLink* links, u32 link_count) {
    Impl& m = *impl_;
    m.rebuilt_tiles = 0;
    if (!vertex_count || !triangle_count) {
        m.release();
        return false;
    }
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
    const int tw = (cfg.width + kTileCells - 1) / kTileCells, th = (cfg.height + kTileCells - 1) / kTileCells;
    // Detour keeps only the tiles that have something in them, so the box may be large (two towns far apart under one
    // Nav Mesh Bounds): the limit is on the tiles in use, below. The grid itself is a list per tile while building.
    if (static_cast<i64>(tw) * th > (1 << 22)) {
        LSD_LOG_WARN("ai", "Navmesh area too large (%d x %d tiles): raise the cell size or use smaller Nav Mesh Bounds", tw, th);
        m.release();
        return false;
    }
    // Each tile's inputs: the triangles over it (with its border), the cutouts and the links starting in it; a hash of them.
    const f32 tile = kTileCells * cfg.cs, border = static_cast<f32>(cfg.walkableRadius + 3) * cfg.cs;
    std::vector<std::vector<int>> buckets(static_cast<usize>(tw * th));
    std::vector<u64> hashes(static_cast<usize>(tw * th), 1469598103934665603ull);
    auto tiles_over = [&](f32 x0, f32 z0, f32 x1, f32 z1, auto&& each) {
        const int ax = std::max(0, static_cast<int>(std::floor((x0 - cfg.bmin[0] - border) / tile)));
        const int az = std::max(0, static_cast<int>(std::floor((z0 - cfg.bmin[2] - border) / tile)));
        const int bx = std::min(tw - 1, static_cast<int>(std::floor((x1 - cfg.bmin[0] + border) / tile)));
        const int bz = std::min(th - 1, static_cast<int>(std::floor((z1 - cfg.bmin[2] + border) / tile)));
        for (int ty = az; ty <= bz; ++ty) {
            for (int tx = ax; tx <= bx; ++tx) each(ty * tw + tx);
        }
    };
    for (u32 t = 0; t < triangle_count; ++t) {
        f32 lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
        for (int k = 0; k < 3; ++k) {
            const f32* v = &verts[static_cast<usize>(tris[t * 3 + static_cast<u32>(k)]) * 3];
            for (int c = 0; c < 3; ++c) lo[c] = std::fmin(lo[c], v[c]), hi[c] = std::fmax(hi[c], v[c]);
        }
        tiles_over(lo[0], lo[2], hi[0], hi[2], [&](int i) {
            buckets[static_cast<usize>(i)].push_back(static_cast<int>(t));
            for (int k = 0; k < 3; ++k) hashes[static_cast<usize>(i)] = mix(hashes[static_cast<usize>(i)], &verts[static_cast<usize>(tris[t * 3 + static_cast<u32>(k)]) * 3], 12);
        });
    }
    for (u32 c = 0; c < cutout_count; ++c) {  // a cutout over a tile changes it
        f32 lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
        for (int k = 0; k < 4; ++k) {
            f32 v[3];
            to_recast(cutouts[c].corners[k], v);
            for (int d = 0; d < 3; ++d) lo[d] = std::fmin(lo[d], v[d]), hi[d] = std::fmax(hi[d], v[d]);
        }
        tiles_over(lo[0], lo[2], hi[0], hi[2], [&](int i) { hashes[static_cast<usize>(i)] = mix(hashes[static_cast<usize>(i)], &cutouts[c], sizeof cutouts[c]); });
    }
    for (u32 l = 0; l < link_count; ++l) {
        f32 v[3];
        to_recast(links[l].start, v);
        tiles_over(v[0], v[2], v[0], v[2], [&](int i) { hashes[static_cast<usize>(i)] = mix(hashes[static_cast<usize>(i)], &links[l], sizeof links[l]); });
    }
    int used = 0;  // tiles with triangles: the ones Detour holds
    for (const std::vector<int>& bucket : buckets) used += bucket.empty() ? 0 : 1;
    if (used > (1 << 14)) {
        LSD_LOG_WARN("ai", "Navmesh too large (%d tiles with floors): raise the cell size or use smaller Nav Mesh Bounds", used);
        m.release();
        return false;
    }
    // The same grid and settings as last time: only the tiles whose inputs changed are rebuilt.
    const bool same_layout = m.mesh && m.tw == tw && m.th == th && std::memcmp(&m.settings, &s, sizeof s) == 0 &&
                             std::memcmp(m.bmin, cfg.bmin, sizeof m.bmin) == 0 && m.hashes.size() == hashes.size() && used <= m.max_tiles;
    if (!same_layout) {
        m.release();
        m.mesh = dtAllocNavMesh();
        dtNavMeshParams params{};
        rcVcopy(params.orig, cfg.bmin);
        params.tileWidth = tile;
        params.tileHeight = tile;
        int tile_bits = 0;  // room for the tiles in use and as many again (things added later), within Detour's 22 bits
        while ((1 << tile_bits) < std::min(tw * th, used * 2 + 1)) ++tile_bits;
        tile_bits = std::min(tile_bits, 14);  // 256 polygons a tile: 32-bit refs have 22 bits for both
        params.maxTiles = 1 << tile_bits;
        m.max_tiles = params.maxTiles;
        params.maxPolys = 1 << (22 - tile_bits);
        if (dtStatusFailed(m.mesh->init(&params))) {
            m.release();
            return false;
        }
        m.tw = tw;
        m.th = th;
        m.settings = s;
        std::memcpy(m.bmin, cfg.bmin, sizeof m.bmin);
        m.hashes.assign(hashes.size(), 0);  // nothing built yet: every tile differs
    }
    // The changed tiles are built on every core (each only reads the inputs), then added to the mesh in order.
    struct Built {
        usize index;
        unsigned char* data = nullptr;
        int size = 0;
        bool ok = false;
    };
    std::vector<Built> changed;
    for (usize i = 0; i < hashes.size(); ++i) {
        if (m.hashes[i] != hashes[i]) changed.push_back({i});
    }
    std::atomic<usize> next{0};
    auto work = [&] {
        for (usize k = next++; k < changed.size(); k = next++) {
            Built& job = changed[k];
            const int tx = static_cast<int>(job.index % static_cast<usize>(tw)), ty = static_cast<int>(job.index / static_cast<usize>(tw));
            job.ok = build_tile(cfg, cfg.bmin, tx, ty, verts, tris, buckets[job.index], s, cutouts, cutout_count, links, link_count, &job.data, &job.size);
        }
    };
    std::vector<std::thread> workers(std::min<usize>(changed.size() / 4, std::max(1u, std::thread::hardware_concurrency()) - 1));
    for (std::thread& t : workers) t = std::thread(work);
    work();
    for (std::thread& t : workers) t.join();
    for (Built& done : changed) {
        m.hashes[done.index] = hashes[done.index];
        const int tx = static_cast<int>(done.index % static_cast<usize>(tw)), ty = static_cast<int>(done.index / static_cast<usize>(tw));
        if (const dtTileRef old = m.mesh->getTileRefAt(tx, ty, 0)) m.mesh->removeTile(old, nullptr, nullptr);
        if (!done.ok) continue;
        ++m.rebuilt_tiles;
        if (done.data && dtStatusFailed(m.mesh->addTile(done.data, done.size, DT_TILE_FREE_DATA, 0, nullptr))) dtFree(done.data);
    }
    bool walkable = false;  // nothing walkable anywhere: no navmesh, as before
    for (int t = 0; t < m.mesh->getMaxTiles() && !walkable; ++t) {
        const dtMeshTile* built = static_cast<const dtNavMesh*>(m.mesh)->getTile(t);
        walkable = built && built->header && built->header->polyCount > 0;
    }
    if (!walkable) {
        m.release();
        return false;
    }
    // The crowd starts again (callers add their agents back); the query follows the mesh.
    if (m.crowd) dtFreeCrowd(m.crowd);
    if (m.query) dtFreeNavMeshQuery(m.query);
    m.query = dtAllocNavMeshQuery();
    m.crowd = dtAllocCrowd();
    if (dtStatusFailed(m.query->init(m.mesh, 2048)) || !m.crowd->init(static_cast<int>(kMaxAgents), 2.0f, m.mesh)) {
        m.release();
        return false;
    }
    return true;
}

u32 NavMesh::last_rebuilt_tiles() const { return impl_->rebuilt_tiles; }

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
            if (poly.getType() == DT_POLYTYPE_OFFMESH_CONNECTION) continue;  // drawn below
            for (int k = 0; k < poly.vertCount && n + 2 <= capacity; ++k) {
                const Vec3 lift{0, 0, 0.05f};  // just above the floor
                out[n++] = from_recast(&tile->verts[poly.verts[k] * 3]) + lift;
                out[n++] = from_recast(&tile->verts[poly.verts[(k + 1) % poly.vertCount] * 3]) + lift;
            }
        }
        for (int c = 0; c < tile->header->offMeshConCount && n + 2 <= capacity; ++c) {
            out[n++] = from_recast(&tile->offMeshCons[c].pos[0]) + Vec3{0, 0, 0.05f};
            out[n++] = from_recast(&tile->offMeshCons[c].pos[3]) + Vec3{0, 0, 0.05f};
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

void NavMesh::clear_agents() {
    if (!impl_->crowd) return;
    for (int i = 0; i < impl_->crowd->getAgentCount(); ++i) {
        if (impl_->crowd->getAgent(i)->active) impl_->crowd->removeAgent(i);
    }
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
