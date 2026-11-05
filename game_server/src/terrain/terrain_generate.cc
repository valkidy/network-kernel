#include "game_server/src/terrain/terrain_generate.h"

#include <DetourNavMesh.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <sstream>

namespace network_example::game_server::terrain {
namespace {

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

std::string turned_down(const NavCheck& c, const LayoutReport& l, const AcceptRules& rules, std::int32_t asked) {
    std::ostringstream why;
    const std::int32_t need = rules.min_placed > 0 ? rules.min_placed : asked;
    if (l.placed < need) why << "placed " << l.placed << "/" << asked << " (need " << need << "); ";
    if (c.reach_fraction() < rules.min_reach_fraction) why << "reach " << c.probes_reached << "/" << c.probes_on_mesh << "; ";
    if (c.unreachable_area_m2 > rules.max_unreachable_m2) why << "unreachable " << c.unreachable_area_m2 << " m2; ";
    if (c.seams_failed > rules.max_seam_failures) why << "seam breaks " << c.seams_failed << "; ";
    return why.str();
}

// Compose + bake + check one recipe. Takes ownership of nothing; returns the navmesh.
dtNavMesh* evaluate(const TerrainRecipe& recipe, const TerrainLibrary& lib, const NavBakeParams& nav,
    HeightGrid* heights, AttemptReport* a, std::string* error) {
    const auto t0 = Clock::now();
    if (!compose(recipe, lib, heights, error)) return nullptr;
    a->compose_ms = ms_since(t0);
    dtNavMesh* mesh = bake_terrain_navmesh(*heights, nav, &a->bake, error);
    if (mesh == nullptr) return nullptr;
    if (!check_terrain_navmesh(*mesh, *heights, recipe, lib, &a->check, error)) {
        dtFreeNavMesh(mesh);
        return nullptr;
    }
    return mesh;
}

}  // namespace

std::int32_t attempt_seed(std::int32_t seed, std::int32_t attempt) {
    if (attempt == 0) return seed;
    std::uint64_t z = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(seed)) << 32) ^ static_cast<std::uint32_t>(attempt);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    z ^= z >> 31;
    return static_cast<std::int32_t>(z & 0x7fffffffu);
}

bool generate_terrain(const GenerateRequest& req, const TerrainLibrary& lib, GenerateResult* out, std::string* error) {
    *out = GenerateResult{};
    struct Stage { const char* name; std::int32_t stamps; GenerateOutcome outcome; };
    std::vector<Stage> stages{{"retry", req.layout.stamp_count, GenerateOutcome::kGenerated}};
    if (req.min_stamp_count > 0 && req.min_stamp_count < req.layout.stamp_count) {
        stages.push_back({"relax", req.min_stamp_count, GenerateOutcome::kRelaxed});
    }
    std::int32_t k = 0;
    for (const Stage& stage : stages) {
        LayoutParams lp = req.layout;
        lp.stamp_count = stage.stamps;
        AcceptRules rules = req.accept;
        if (rules.min_placed == 0) rules.min_placed = stage.stamps;
        rules.min_placed = std::min(rules.min_placed, stage.stamps);
        for (std::int32_t i = 0; i < req.max_retries; ++i, ++k) {
            AttemptReport a;
            a.stage = stage.name;
            a.seed = attempt_seed(req.seed, k);
            TerrainRecipe r = req.base;
            r.seed = a.seed;
            const auto t0 = Clock::now();
            if (!layout_stamps(&r, lib, lp, &a.layout, error)) return false;
            a.layout_ms = ms_since(t0);
            HeightGrid h;
            dtNavMesh* mesh = evaluate(r, lib, req.nav, &h, &a, error);
            if (mesh == nullptr) return false;
            a.reason = turned_down(a.check, a.layout, rules, stage.stamps);
            a.accepted = a.reason.empty();
            out->attempts.push_back(a);
            if (a.accepted) {
                out->outcome = stage.outcome;
                out->recipe = std::move(r);
                out->heights = std::move(h);
                out->hash = height_grid_hash(out->heights);
                out->navmesh = mesh;
                return true;
            }
            dtFreeNavMesh(mesh);
        }
    }
    if (req.fallback.has_value()) {
        AttemptReport a;
        a.stage = "fallback";
        a.seed = req.fallback->seed;
        HeightGrid h;
        dtNavMesh* mesh = evaluate(*req.fallback, lib, req.nav, &h, &a, error);
        if (mesh == nullptr) return false;
        const std::uint64_t hash = height_grid_hash(h);
        if (hash != req.fallback_hash) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "fallback recipe composes to %016llx, recorded %016llx",
                static_cast<unsigned long long>(hash), static_cast<unsigned long long>(req.fallback_hash));
            a.reason = buf;
        }
        a.accepted = a.reason.empty();
        out->attempts.push_back(a);
        if (a.accepted) {
            out->outcome = GenerateOutcome::kFallback;
            out->recipe = *req.fallback;
            out->heights = std::move(h);
            out->hash = hash;
            out->navmesh = mesh;
            return true;
        }
        dtFreeNavMesh(mesh);
    }
    out->outcome = GenerateOutcome::kFailed;
    *error = "no terrain passed its checks (" + std::to_string(out->attempts.size()) + " attempts)";
    return false;
}

std::string format_attempt(const AttemptReport& a, const TerrainLibrary& lib) {
    (void)lib;
    std::ostringstream s;
    s << a.stage << " seed=" << a.seed << (a.accepted ? " ACCEPTED" : " turned down") << ": placed "
      << a.layout.placed << "/" << a.layout.picked;
    if (!a.layout.unplaced.empty()) {
        s << " (no site:";
        for (const std::string& n : a.layout.unplaced) s << " " << n;
        s << ")";
    }
    s << ", sites refused ring " << a.layout.rejected_ring << " / overlap " << a.layout.rejected_overlap << " / cliff "
      << a.layout.rejected_cliff << "; islands " << a.check.islands << ", reach " << a.check.probes_reached << "/"
      << a.check.probes_on_mesh << " (" << a.check.unreachable_clusters << " clusters, " << a.check.unreachable_area_m2
      << " m2), seams " << (a.check.seams_tested - a.check.seams_no_mesh - a.check.seams_failed) << "/"
      << (a.check.seams_tested - a.check.seams_no_mesh) << " (" << a.check.seams_no_mesh << " no mesh, "
      << a.check.seams_detoured << " detours); layout "
      << static_cast<int>(a.layout_ms) << " ms, compose " << static_cast<int>(a.compose_ms) << " ms, bake "
      << static_cast<int>(a.bake.bake_ms) << " ms (" << a.bake.tiles << " tiles, " << a.bake.polys << " polys), check "
      << static_cast<int>(a.check.check_ms) << " ms";
    if (!a.reason.empty()) s << " -- " << a.reason;
    return s.str();
}

}  // namespace network_example::game_server::terrain
