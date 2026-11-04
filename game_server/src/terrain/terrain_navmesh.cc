#include "game_server/src/terrain/terrain_navmesh.h"

#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <DetourNavMeshQuery.h>
#include <Recast.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <numeric>
#include <thread>

namespace network_example::game_server::terrain {
namespace {

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

struct RecastTile {
    std::vector<unsigned char> data;   // dtCreateNavMeshData output (dtAlloc'd copy made at add time)
};

template <typename T, void (*Free)(T*)>
struct Freer {
    void operator()(T* p) const { if (p) Free(p); }
};
using HeightfieldPtr = std::unique_ptr<rcHeightfield, Freer<rcHeightfield, rcFreeHeightField>>;
using CompactPtr = std::unique_ptr<rcCompactHeightfield, Freer<rcCompactHeightfield, rcFreeCompactHeightfield>>;
using ContourPtr = std::unique_ptr<rcContourSet, Freer<rcContourSet, rcFreeContourSet>>;
using PolyMeshPtr = std::unique_ptr<rcPolyMesh, Freer<rcPolyMesh, rcFreePolyMesh>>;
using DetailPtr = std::unique_ptr<rcPolyMeshDetail, Freer<rcPolyMeshDetail, rcFreePolyMeshDetail>>;

constexpr float kVoxelShift = 0.5f;   // of a cell

struct BakeContext {
    const HeightGrid* grid = nullptr;
    const NavBakeParams* p = nullptr;
    float ox = 0, oz = 0;           // world position of sample (0, 0)
    std::int64_t ox_mm = 0, oz_mm = 0;
    float ymin = 0, ymax = 0;
    std::int32_t tiles_x = 0, tiles_z = 0;
};

// The nearest float to the exact position (integer mm), not an accumulated
// i * 0.3f: Recast's cell walls are at bmin + k * cs, and a vertex a few ulps to
// one side or the other of a wall decides which column a thin feature lands in.
float world_x(const BakeContext& c, std::int32_t i) { return static_cast<float>(static_cast<double>(c.ox_mm + static_cast<std::int64_t>(i) * kCellMm) / 1000.0); }
float world_z(const BakeContext& c, std::int32_t j) { return static_cast<float>(static_cast<double>(c.oz_mm + static_cast<std::int64_t>(j) * kCellMm) / 1000.0); }

// One tile, or an empty result when it has no walkable polygon.
bool bake_tile(const BakeContext& c, std::int32_t tx, std::int32_t tz, RecastTile* out, std::string* error) {
    const NavBakeParams& p = *c.p;
    const HeightGrid& g = *c.grid;
    rcConfig cfg{};
    cfg.cs = p.cell_size;
    cfg.ch = p.cell_height;
    cfg.walkableSlopeAngle = p.walkable_slope_degrees;
    cfg.walkableHeight = static_cast<int>(std::ceil(p.agent_height / cfg.ch));
    cfg.walkableClimb = static_cast<int>(std::floor(p.agent_max_climb / cfg.ch));
    cfg.walkableRadius = static_cast<int>(std::ceil(p.agent_radius / cfg.cs));
    cfg.maxEdgeLen = static_cast<int>(p.edge_max_length / cfg.cs);
    cfg.maxSimplificationError = p.edge_max_error;
    cfg.minRegionArea = static_cast<int>(p.region_min_size * p.region_min_size);
    cfg.mergeRegionArea = static_cast<int>(p.region_merge_size * p.region_merge_size);
    cfg.maxVertsPerPoly = p.max_vertices_per_polygon;
    cfg.detailSampleDist = cfg.cs * p.detail_sample_distance_factor;
    cfg.detailSampleMaxError = cfg.ch * p.detail_sample_max_error_factor;
    cfg.tileSize = p.tile_cells;
    cfg.borderSize = cfg.walkableRadius + 3;
    cfg.width = cfg.tileSize + 2 * cfg.borderSize;
    cfg.height = cfg.tileSize + 2 * cfg.borderSize;
    const float tile_m = static_cast<float>(p.tile_cells) * cfg.cs;
    // Voxel columns centred on the samples, not walled at them: with a sample
    // exactly on a cell wall, float noise in the vertex decided which column a
    // thin ramp foot fell in, and the same stamp connected at one map position
    // and not at another (measured: Depression_0001 at x = 12 / 24 / 36 m).
    const float shift = kVoxelShift * cfg.cs;
    cfg.bmin[0] = c.ox + shift + static_cast<float>(tx) * tile_m - static_cast<float>(cfg.borderSize) * cfg.cs;
    cfg.bmin[1] = c.ymin;
    cfg.bmin[2] = c.oz + shift + static_cast<float>(tz) * tile_m - static_cast<float>(cfg.borderSize) * cfg.cs;
    cfg.bmax[0] = c.ox + shift + static_cast<float>(tx + 1) * tile_m + static_cast<float>(cfg.borderSize) * cfg.cs;
    cfg.bmax[1] = c.ymax;
    cfg.bmax[2] = c.oz + shift + static_cast<float>(tz + 1) * tile_m + static_cast<float>(cfg.borderSize) * cfg.cs;

    // The heightfield cells under the tile and its border, as triangles.
    const std::int32_t cx = g.samples_x - 1, cz = g.samples_z - 1;
    const std::int32_t i0 = std::max(0, tx * p.tile_cells - cfg.borderSize - 1);
    const std::int32_t i1 = std::min(cx, (tx + 1) * p.tile_cells + cfg.borderSize + 1);
    const std::int32_t j0 = std::max(0, tz * p.tile_cells - cfg.borderSize - 1);
    const std::int32_t j1 = std::min(cz, (tz + 1) * p.tile_cells + cfg.borderSize + 1);
    if (i0 >= i1 || j0 >= j1) {
        return true;
    }
    const std::int32_t vx = i1 - i0 + 1, vz = j1 - j0 + 1;
    std::vector<float> verts(static_cast<std::size_t>(vx) * static_cast<std::size_t>(vz) * 3);
    for (std::int32_t j = 0; j < vz; ++j)
        for (std::int32_t i = 0; i < vx; ++i) {
            float* v = &verts[(static_cast<std::size_t>(j) * static_cast<std::size_t>(vx) + static_cast<std::size_t>(i)) * 3];
            v[0] = world_x(c, i0 + i);
            v[1] = static_cast<float>(g.at(i0 + i, j0 + j)) / 100.0f;
            v[2] = world_z(c, j0 + j);
        }
    std::vector<int> tris;
    tris.reserve(static_cast<std::size_t>(vx - 1) * static_cast<std::size_t>(vz - 1) * 6);
    auto vi = [vx](std::int32_t i, std::int32_t j) { return j * vx + i; };
    for (std::int32_t j = 0; j + 1 < vz; ++j)
        for (std::int32_t i = 0; i + 1 < vx; ++i) {
            // Jolt's split: (x,y)-(x,y+1)-(x+1,y+1) and (x,y)-(x+1,y+1)-(x+1,y); both face up.
            tris.insert(tris.end(), {vi(i, j), vi(i, j + 1), vi(i + 1, j + 1)});
            tris.insert(tris.end(), {vi(i, j), vi(i + 1, j + 1), vi(i + 1, j)});
        }
    const int ntris = static_cast<int>(tris.size() / 3), nverts = vx * vz;

    rcContext ctx(false);
    HeightfieldPtr hf(rcAllocHeightfield());
    if (!hf || !rcCreateHeightfield(&ctx, *hf, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch)) {
        *error = "Recast heightfield creation failed";
        return false;
    }
    std::vector<unsigned char> areas(static_cast<std::size_t>(ntris), 0);
    rcMarkWalkableTriangles(&ctx, cfg.walkableSlopeAngle, verts.data(), nverts, tris.data(), ntris, areas.data());
    if (!rcRasterizeTriangles(&ctx, verts.data(), nverts, tris.data(), areas.data(), ntris, *hf, cfg.walkableClimb)) {
        *error = "Recast rasterization failed";
        return false;
    }
    rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *hf);
    rcFilterLedgeSpans(&ctx, cfg.walkableHeight, cfg.walkableClimb, *hf);
    rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *hf);
    CompactPtr chf(rcAllocCompactHeightfield());
    if (!chf || !rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb, *hf, *chf) ||
        !rcErodeWalkableArea(&ctx, cfg.walkableRadius, *chf) || !rcBuildDistanceField(&ctx, *chf) ||
        !rcBuildRegions(&ctx, *chf, cfg.borderSize, cfg.minRegionArea, cfg.mergeRegionArea)) {
        *error = "Recast compact heightfield / regions failed";
        return false;
    }
    ContourPtr cset(rcAllocContourSet());
    PolyMeshPtr pmesh(rcAllocPolyMesh());
    DetailPtr dmesh(rcAllocPolyMeshDetail());
    if (!cset || !pmesh || !dmesh || !rcBuildContours(&ctx, *chf, cfg.maxSimplificationError, cfg.maxEdgeLen, *cset) ||
        !rcBuildPolyMesh(&ctx, *cset, cfg.maxVertsPerPoly, *pmesh) ||
        !rcBuildPolyMeshDetail(&ctx, *pmesh, *chf, cfg.detailSampleDist, cfg.detailSampleMaxError, *dmesh)) {
        *error = "Recast polygon / detail mesh failed";
        return false;
    }
    if (pmesh->npolys == 0) {
        return true;
    }
    for (int k = 0; k < pmesh->npolys; ++k) {
        if (pmesh->areas[k] == RC_WALKABLE_AREA) pmesh->areas[k] = 0;
        pmesh->flags[k] = 1;
    }
    dtNavMeshCreateParams params{};
    params.verts = pmesh->verts;
    params.vertCount = pmesh->nverts;
    params.polys = pmesh->polys;
    params.polyAreas = pmesh->areas;
    params.polyFlags = pmesh->flags;
    params.polyCount = pmesh->npolys;
    params.nvp = pmesh->nvp;
    params.detailMeshes = dmesh->meshes;
    params.detailVerts = dmesh->verts;
    params.detailVertsCount = dmesh->nverts;
    params.detailTris = dmesh->tris;
    params.detailTriCount = dmesh->ntris;
    params.walkableHeight = p.agent_height;
    params.walkableRadius = p.agent_radius;
    params.walkableClimb = p.agent_max_climb;
    params.tileX = tx;
    params.tileY = tz;
    params.tileLayer = 0;
    rcVcopy(params.bmin, pmesh->bmin);
    rcVcopy(params.bmax, pmesh->bmax);
    params.cs = cfg.cs;
    params.ch = cfg.ch;
    params.buildBvTree = true;
    unsigned char* data = nullptr;
    int size = 0;
    if (!dtCreateNavMeshData(&params, &data, &size)) {
        *error = "Detour tile data creation failed";
        return false;
    }
    out->data.assign(data, data + size);
    dtFree(data);
    return true;
}

std::uint32_t next_pow2(std::uint32_t v) {
    std::uint32_t r = 1;
    while (r < v) r <<= 1;
    return r;
}

std::uint32_t ilog2(std::uint32_t v) {
    std::uint32_t r = 0;
    while (v >>= 1) ++r;
    return r;
}

}  // namespace

dtNavMesh* bake_terrain_navmesh(const HeightGrid& grid, const NavBakeParams& p, NavBakeStats* stats, std::string* error) {
    const auto t0 = Clock::now();
    if (grid.samples_x < 2 || grid.samples_z < 2 || p.tile_cells < 8) {
        *error = "navmesh bake input out of range";
        return nullptr;
    }
    BakeContext c;
    c.grid = &grid;
    c.p = &p;
    c.ox_mm = -static_cast<std::int64_t>(grid.samples_x - 1) * kCellMm / 2;
    c.oz_mm = -static_cast<std::int64_t>(grid.samples_z - 1) * kCellMm / 2;
    c.ox = static_cast<float>(static_cast<double>(c.ox_mm) / 1000.0);
    c.oz = static_cast<float>(static_cast<double>(c.oz_mm) / 1000.0);
    const auto [lo, hi] = std::minmax_element(grid.cm.begin(), grid.cm.end());
    c.ymin = static_cast<float>(*lo) / 100.0f - 1.0f;
    c.ymax = static_cast<float>(*hi) / 100.0f + p.agent_height + 1.0f;
    c.tiles_x = (grid.samples_x - 2) / p.tile_cells + 1;
    c.tiles_z = (grid.samples_z - 2) / p.tile_cells + 1;
    const std::int32_t ntiles = c.tiles_x * c.tiles_z;

    std::vector<RecastTile> tiles(static_cast<std::size_t>(ntiles));
    std::vector<std::string> errors(static_cast<std::size_t>(ntiles));
    std::atomic<std::int32_t> next{0};
    std::uint32_t threads = p.threads != 0 ? p.threads : std::max(1u, std::thread::hardware_concurrency());
    threads = std::min<std::uint32_t>(threads, static_cast<std::uint32_t>(ntiles));
    auto work = [&]() {
        for (std::int32_t k = next++; k < ntiles; k = next++) {
            bake_tile(c, k % c.tiles_x, k / c.tiles_x, &tiles[static_cast<std::size_t>(k)], &errors[static_cast<std::size_t>(k)]);
        }
    };
    std::vector<std::thread> pool;
    for (std::uint32_t t = 1; t < threads; ++t) pool.emplace_back(work);
    work();
    for (std::thread& t : pool) t.join();
    for (const std::string& e : errors) {
        if (!e.empty()) { *error = e; return nullptr; }
    }

    dtNavMeshParams np{};
    np.orig[0] = c.ox + kVoxelShift * p.cell_size;   // the tile grid moves with the voxels
    np.orig[1] = 0.0f;
    np.orig[2] = c.oz + kVoxelShift * p.cell_size;
    np.tileWidth = np.tileHeight = static_cast<float>(p.tile_cells) * p.cell_size;
    const std::uint32_t max_tiles = next_pow2(static_cast<std::uint32_t>(ntiles));
    const std::uint32_t tile_bits = ilog2(max_tiles);
    if (tile_bits > 14) {
        *error = "navmesh has too many tiles for 32-bit poly refs";
        return nullptr;
    }
    np.maxTiles = static_cast<int>(max_tiles);
    np.maxPolys = 1 << (22 - tile_bits);
    dtNavMesh* nav = dtAllocNavMesh();
    if (nav == nullptr || dtStatusFailed(nav->init(&np))) {
        dtFreeNavMesh(nav);
        *error = "Detour navmesh init failed";
        return nullptr;
    }
    std::uint32_t added = 0, polys = 0;
    for (RecastTile& t : tiles) {
        if (t.data.empty()) continue;
        auto* data = static_cast<unsigned char*>(dtAlloc(static_cast<int>(t.data.size()), DT_ALLOC_PERM));
        std::copy(t.data.begin(), t.data.end(), data);
        const auto* header = reinterpret_cast<const dtMeshHeader*>(data);
        if (header->polyCount > np.maxPolys) {
            dtFree(data);
            dtFreeNavMesh(nav);
            *error = "navmesh tile has more polygons than its poly ref bits allow";
            return nullptr;
        }
        polys += static_cast<std::uint32_t>(header->polyCount);
        if (dtStatusFailed(nav->addTile(data, static_cast<int>(t.data.size()), DT_TILE_FREE_DATA, 0, nullptr))) {
            dtFree(data);
            dtFreeNavMesh(nav);
            *error = "Detour addTile failed";
            return nullptr;
        }
        ++added;
        std::vector<unsigned char>().swap(t.data);
    }
    if (stats != nullptr) {
        stats->tiles = added;
        stats->polys = polys;
        stats->bake_ms = ms_since(t0);
    }
    return nav;
}

namespace {

void put32(std::vector<std::uint8_t>* b, std::uint32_t v) {
    for (int k = 0; k < 4; ++k) b->push_back(static_cast<std::uint8_t>(v >> (8 * k)));
}

std::uint32_t get32(const std::vector<std::uint8_t>& b, std::size_t* p) {
    std::uint32_t v = 0;
    for (int k = 0; k < 4; ++k) v |= static_cast<std::uint32_t>(b[(*p)++]) << (8 * k);
    return v;
}

}  // namespace

std::vector<std::uint8_t> write_navmesh_tiles(const dtNavMesh& nav) {
    std::vector<std::uint8_t> b{'N', 'K', 'T', 'N'};
    put32(&b, 1);
    const dtNavMeshParams* np = nav.getParams();
    for (const float f : {np->orig[0], np->orig[1], np->orig[2], np->tileWidth, np->tileHeight}) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &f, sizeof(bits));
        put32(&b, bits);
    }
    put32(&b, static_cast<std::uint32_t>(np->maxTiles));
    put32(&b, static_cast<std::uint32_t>(np->maxPolys));
    std::vector<const dtMeshTile*> tiles;
    for (int t = 0; t < nav.getMaxTiles(); ++t) {
        const dtMeshTile* tile = nav.getTile(t);
        if (tile != nullptr && tile->header != nullptr && tile->data != nullptr) tiles.push_back(tile);
    }
    put32(&b, static_cast<std::uint32_t>(tiles.size()));
    for (const dtMeshTile* tile : tiles) {
        put32(&b, static_cast<std::uint32_t>(tile->dataSize));
        b.insert(b.end(), tile->data, tile->data + tile->dataSize);
    }
    return b;
}

bool is_navmesh_tiles(const std::vector<std::uint8_t>& b) {
    return b.size() >= 8 && std::memcmp(b.data(), "NKTN", 4) == 0;
}

dtNavMesh* read_navmesh_tiles(const std::vector<std::uint8_t>& b, std::string* error) {
    constexpr std::size_t kHeader = 4 + 4 + 5 * 4 + 4 + 4 + 4;
    if (!is_navmesh_tiles(b) || b.size() < kHeader) {
        *error = "not a terrain navmesh (NKTN)";
        return nullptr;
    }
    std::size_t p = 4;
    if (get32(b, &p) != 1) {
        *error = "terrain navmesh version is not 1";
        return nullptr;
    }
    dtNavMeshParams np{};
    float f[5];
    for (float& v : f) {
        const std::uint32_t bits = get32(b, &p);
        std::memcpy(&v, &bits, sizeof(v));
    }
    np.orig[0] = f[0]; np.orig[1] = f[1]; np.orig[2] = f[2];
    np.tileWidth = f[3]; np.tileHeight = f[4];
    np.maxTiles = static_cast<int>(get32(b, &p));
    np.maxPolys = static_cast<int>(get32(b, &p));
    const std::uint32_t n = get32(b, &p);
    dtNavMesh* nav = dtAllocNavMesh();
    if (nav == nullptr || dtStatusFailed(nav->init(&np))) {
        dtFreeNavMesh(nav);
        *error = "terrain navmesh init failed";
        return nullptr;
    }
    for (std::uint32_t k = 0; k < n; ++k) {
        if (p + 4 > b.size()) { dtFreeNavMesh(nav); *error = "terrain navmesh is truncated"; return nullptr; }
        const std::uint32_t size = get32(b, &p);
        if (p + size > b.size()) { dtFreeNavMesh(nav); *error = "terrain navmesh is truncated"; return nullptr; }
        auto* data = static_cast<unsigned char*>(dtAlloc(static_cast<int>(size), DT_ALLOC_PERM));
        std::memcpy(data, b.data() + p, size);
        p += size;
        if (dtStatusFailed(nav->addTile(data, static_cast<int>(size), DT_TILE_FREE_DATA, 0, nullptr))) {
            dtFree(data);
            dtFreeNavMesh(nav);
            *error = "terrain navmesh addTile failed";
            return nullptr;
        }
    }
    return nav;
}

namespace {

struct Components {
    std::map<dtPolyRef, std::uint32_t> id;      // poly -> component (ordered: deterministic)
    std::vector<double> area;                   // per component, m2
};

double poly_area(const dtMeshTile* tile, const dtPoly& poly) {
    double a = 0;
    for (int k = 0; k < poly.vertCount; ++k) {
        const float* p = &tile->verts[poly.verts[k] * 3];
        const float* q = &tile->verts[poly.verts[(k + 1) % poly.vertCount] * 3];
        a += static_cast<double>(p[0]) * q[2] - static_cast<double>(q[0]) * p[2];
    }
    return std::fabs(a) / 2.0;
}

Components find_components(const dtNavMesh& nav) {
    Components c;
    std::vector<dtPolyRef> stack;
    for (int t = 0; t < nav.getMaxTiles(); ++t) {
        const dtMeshTile* tile = nav.getTile(t);
        if (tile == nullptr || tile->header == nullptr) continue;
        const dtPolyRef base = nav.getPolyRefBase(tile);
        for (int p = 0; p < tile->header->polyCount; ++p) {
            if (tile->polys[p].getType() != DT_POLYTYPE_GROUND) continue;
            const dtPolyRef start = base | static_cast<dtPolyRef>(p);
            if (c.id.count(start)) continue;
            const std::uint32_t comp = static_cast<std::uint32_t>(c.area.size());
            c.area.push_back(0.0);
            stack.assign(1, start);
            c.id[start] = comp;
            while (!stack.empty()) {
                const dtPolyRef ref = stack.back();
                stack.pop_back();
                const dtMeshTile* rt = nullptr;
                const dtPoly* poly = nullptr;
                nav.getTileAndPolyByRefUnsafe(ref, &rt, &poly);
                c.area[comp] += poly_area(rt, *poly);
                for (unsigned int l = poly->firstLink; l != DT_NULL_LINK; l = rt->links[l].next) {
                    const dtPolyRef nb = rt->links[l].ref;
                    if (nb != 0 && !c.id.count(nb)) {
                        c.id[nb] = comp;
                        stack.push_back(nb);
                    }
                }
            }
        }
    }
    return c;
}

double grid_height_m(const HeightGrid& g, double x, double z) {
    const double fx = std::clamp(x / 0.3, 0.0, static_cast<double>(g.samples_x - 1));
    const double fz = std::clamp(z / 0.3, 0.0, static_cast<double>(g.samples_z - 1));
    const std::int32_t x0 = std::min(static_cast<std::int32_t>(fx), g.samples_x - 2);
    const std::int32_t z0 = std::min(static_cast<std::int32_t>(fz), g.samples_z - 2);
    const double tx = fx - x0, tz = fz - z0;
    const double a = g.at(x0, z0) + (g.at(x0 + 1, z0) - g.at(x0, z0)) * tx;
    const double b = g.at(x0, z0 + 1) + (g.at(x0 + 1, z0 + 1) - g.at(x0, z0 + 1)) * tx;
    return (a + (b - a) * tz) / 100.0;
}

}  // namespace

bool check_terrain_navmesh(const dtNavMesh& nav, const HeightGrid& g, const TerrainRecipe& recipe,
    const TerrainLibrary& lib, NavCheck* out, std::string* error) {
    const double agent_max_climb = NavBakeParams{}.agent_max_climb;
    const auto t0 = Clock::now();
    *out = NavCheck{};
    const Components comp = find_components(nav);
    out->islands = static_cast<std::uint32_t>(comp.area.size());
    if (comp.area.empty()) {
        *error = "navmesh has no polygons";
        return false;
    }
    const std::uint32_t main = static_cast<std::uint32_t>(std::max_element(comp.area.begin(), comp.area.end()) - comp.area.begin());
    out->island_areas_m2 = comp.area;
    std::sort(out->island_areas_m2.begin(), out->island_areas_m2.end(), std::greater<>());

    std::unique_ptr<dtNavMeshQuery, void (*)(dtNavMeshQuery*)> q(dtAllocNavMeshQuery(), dtFreeNavMeshQuery);
    if (!q || dtStatusFailed(q->init(&nav, 4096))) {
        *error = "navmesh query init failed";
        return false;
    }
    dtQueryFilter filter;
    const double size_x = (g.samples_x - 1) * 0.3, size_z = (g.samples_z - 1) * 0.3;
    const double ox = -size_x / 2, oz = -size_z / 2;
    const float snap[3] = {1.5f, 1.5f, 1.5f};
    auto snap_to = [&](double x, double z, dtPolyRef* ref, float* pt) {
        const float c[3] = {static_cast<float>(ox + x), static_cast<float>(grid_height_m(g, x, z)), static_cast<float>(oz + z)};
        *ref = 0;
        return dtStatusSucceed(q->findNearestPoly(c, snap, &filter, ref, pt)) && *ref != 0;
    };
    auto component_of = [&](dtPolyRef r) {
        const auto it = comp.id.find(r);
        return it == comp.id.end() ? ~0u : it->second;
    };

    // Reach.
    const std::int32_t n = static_cast<std::int32_t>(std::floor(size_x));
    const std::int32_t nz = static_cast<std::int32_t>(std::floor(size_z));
    out->probe_nx = n;
    out->probe_state.assign(static_cast<std::size_t>(n) * static_cast<std::size_t>(nz), 0);
    for (std::int32_t zi = 0; zi < nz; ++zi)
        for (std::int32_t xi = 0; xi < n; ++xi) {
            const double x = xi + 0.5, z = zi + 0.5;
            dtPolyRef ref = 0;
            float pt[3];
            if (!snap_to(x, z, &ref, pt)) continue;
            if (std::fabs(pt[0] - (ox + x)) > 0.5 || std::fabs(pt[2] - (oz + z)) > 0.5) continue;
            ++out->probes_on_mesh;
            const bool ok = component_of(ref) == main;
            out->probes_reached += ok;
            out->probe_state[static_cast<std::size_t>(zi) * static_cast<std::size_t>(n) + static_cast<std::size_t>(xi)] = ok ? 1 : 2;
        }
    // Unreachable clusters (8-connected).
    {
        std::vector<std::uint8_t> seen(out->probe_state.size(), 0);
        std::vector<std::int32_t> stack;
        for (std::size_t k = 0; k < out->probe_state.size(); ++k) {
            if (out->probe_state[k] != 2 || seen[k]) continue;
            ++out->unreachable_clusters;
            stack.assign(1, static_cast<std::int32_t>(k));
            seen[k] = 1;
            while (!stack.empty()) {
                const std::int32_t c = stack.back();
                stack.pop_back();
                out->unreachable_area_m2 += 1.0;
                const std::int32_t cx = c % n, cz = c / n;
                for (int dz = -1; dz <= 1; ++dz)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const std::int32_t x = cx + dx, z = cz + dz;
                        if (x < 0 || z < 0 || x >= n || z >= nz) continue;
                        const std::size_t kk = static_cast<std::size_t>(z) * static_cast<std::size_t>(n) + static_cast<std::size_t>(x);
                        if (out->probe_state[kk] == 2 && !seen[kk]) { seen[kk] = 1; stack.push_back(static_cast<std::int32_t>(kk)); }
                    }
            }
        }
    }

    // Seams.
    dtPolyRef path[256];
    float straight[3 * 64];
    for (const TerrainPlacement& p : recipe.placements) {
        const double e = lib.stamps[p.stamp].cells_x * 0.3;
        const double px = p.x_cells * 0.3, pz = p.z_cells * 0.3;
        for (int side = 0; side < 4; ++side)
            for (double t = 1.0; t < e - 0.99; t += 2.0) {
                double ex = 0, ez = 0, nx = 0, nzz = 0;   // edge point, outward normal
                switch (side) {
                case 0: ex = px + t; ez = pz; nx = 0; nzz = -1; break;
                case 1: ex = px + e; ez = pz + t; nx = 1; nzz = 0; break;
                case 2: ex = px + t; ez = pz + e; nx = 0; nzz = 1; break;
                default: ex = px; ez = pz + t; nx = -1; nzz = 0; break;
                }
                const double ix = ex - nx * 1.5, iz = ez - nzz * 1.5, xo = ex + nx * 1.5, zo = ez + nzz * 1.5;
                if (xo < 1 || zo < 1 || xo > size_x - 1 || zo > size_z - 1) continue;
                ++out->seams_tested;
                dtPolyRef ra = 0, rb = 0;
                float a[3], b[3];
                if (!snap_to(ix, iz, &ra, a) || !snap_to(xo, zo, &rb, b)) { ++out->seams_no_mesh; continue; }
                // The pair tests a crossing only if the inside point is on ground at
                // the stamp's border height. A wall or ledge within 1.5 m of the
                // border puts it on top of that feature instead (Compose Test Map
                // counts these as "no navmesh on one side", because Unity's bake
                // erodes such thin tops away).
                if (std::fabs(a[1] - p.base_cm / 100.0) > agent_max_climb) { ++out->seams_no_mesh; continue; }
                int npath = 0;
                const dtStatus st = q->findPath(ra, rb, a, b, &filter, path, &npath, 256);
                bool ok = dtStatusSucceed(st) && !dtStatusDetail(st, DT_PARTIAL_RESULT) && npath > 0 && path[npath - 1] == rb;
                double len = 0;
                if (ok) {
                    int ns = 0;
                    ok = dtStatusSucceed(q->findStraightPath(a, b, path, npath, straight, nullptr, nullptr, &ns, 64)) && ns > 0;
                    for (int k = 1; k < ns; ++k) len += std::sqrt(rcVdistSqr(&straight[3 * (k - 1)], &straight[3 * k]));
                }
                if (!ok || len > 7.0) {
                    if (std::getenv("TERRAIN_SEAM_TRACE") != nullptr)
                        std::fprintf(stderr, "seam %s: stamp %u side %d at map (%.1f, %.1f): len %.1f m, inside y %.2f, outside y %.2f\n",
                            ok ? "detour" : "break", p.stamp, side, ex, ez, len, a[1], b[1]);
                    if (ok) {
                        ++out->seams_detoured;
                    } else {
                        ++out->seams_failed;
                        out->seam_failures_xz.push_back(static_cast<float>((ix + xo) / 2));
                        out->seam_failures_xz.push_back(static_cast<float>((iz + zo) / 2));
                    }
                }
            }
    }
    out->check_ms = ms_since(t0);
    return true;
}

}  // namespace network_example::game_server::terrain
