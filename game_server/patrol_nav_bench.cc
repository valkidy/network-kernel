// Whether a patrol route can be a straight line.
//
// The patrol design under discussion spawns a squad somewhere in a rectangle
// and walks it along a chord to somewhere else. Nothing in this repo queries a
// navmesh at runtime -- Detour appears only in the offline bake -- so the first
// question is not how to path, it is whether pathing is needed at all: if a
// straight chord between two walkable points is itself walkable often enough,
// the whole route layer collapses into two points and a direction vector.
//
// The navmesh needed to answer that already exists. mesh_asset_bakes emits
// generated/recast/<stem>.navmesh beside the .joltmesh, the bundle genrule
// copies it into mesh_assets/recast/, and the payload is raw dtCreateNavMeshData
// output. This benchmark loads it the way game_server would and measures three
// things per terrain:
//
//   on-navmesh     -- what fraction of a uniform sample over the mesh bounds
//                     lands on walkable ground. Reported at two radii, because
//                     they answer two different questions: `exact` is the odds
//                     that a blindly chosen point in an authored rectangle is
//                     already valid, and `snapped` is the odds that it is close
//                     enough to be pulled onto the navmesh. The gap between
//                     them is the cost of not having a snap step.
//   connected      -- of pairs of walkable points, how many findPath joins
//                     completely. A partial path means the two ends are in
//                     disconnected regions.
//   straight-walk  -- how many of those pairs can be walked in a straight line
//                     without leaving the navmesh, measured with Detour's
//                     surface raycast. THIS is the number the route design
//                     hangs on.
//   detour ratio   -- path length over straight-line length, measured ONLY over
//                     the pairs whose straight line fails. Averaged over every
//                     pair it would be dominated by the ones already at 1.00
//                     and would say nothing; what the design needs to know is
//                     what going around actually costs when going straight is
//                     not an option.
//
// Bucketed by distance, because a 10 m chord and a 120 m chord are not the same
// question and a single aggregate number would hide that patrol routes are the
// long ones.
//
// Distances are horizontal (XZ) on both sides of every ratio: the design
// question is about ground routes, and counting the climb of a slope as route
// length would make hilly terrain look like it detours when it goes straight.
//
// Reports measurements rather than asserting them: what counts as an acceptable
// straight-walk rate is a design decision, and the answer is a property of the
// authored terrain, not of the code. Run it with
//   bazel run -c opt //game_server:patrol_nav_bench

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <DetourAlloc.h>
#include <DetourNavMesh.h>
// dtNavMeshHeaderSwapEndian / dtNavMeshDataSwapEndian live in the builder
// header, not in DetourNavMesh.h.
#include <DetourNavMeshBuilder.h>
#include <DetourNavMeshQuery.h>
#include <DetourStatus.h>

#include "game_server/gameplay_config.h"

namespace {

void require_impl(bool condition, int line, const char* text) {
    if (condition) {
        return;
    }
    std::fprintf(stderr, "require failed at line %d: %s\n", line, text);
    std::abort();
}

#define require(expr) require_impl(static_cast<bool>(expr), __LINE__, #expr)

// geometry/public/mesh_bake.h owns these; duplicated rather than depended on so
// that the bench does not pull Recast and the Jolt bake path in behind Detour.
constexpr std::size_t kMeshArtifactHeaderSize = 88;
constexpr std::array<std::uint8_t, 8> kRecastMagic = {
    'N', 'K', 'M', 'R', 'C', 'S', 'T', '1'};
constexpr std::uint32_t kLittleEndian = 1;

// How far off the navmesh a sampled point may be and still count as placeable.
// Roughly one agent radius: close enough that a spawn there can be snapped down
// onto walkable ground rather than rejected.
constexpr float kSnapHalfExtentMeters = 1.0f;
// Small enough to mean "the point is already on the navmesh" without demanding
// exact float agreement with a polygon edge.
constexpr float kExactHalfExtentMeters = 0.1f;
constexpr std::size_t kSamplePoints = 4000;
constexpr std::size_t kRoutePairs = 2000;
// A 120 m route over 0.3 m cells crosses a lot of polygons. Sized so that
// hitting the ceiling is a bug worth reporting rather than a normal outcome.
constexpr int kMaxPathPolys = 1024;
constexpr int kMaxStraightPathPoints = 512;

std::filesystem::path runfiles_root() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    const char* test_workspace = std::getenv("TEST_WORKSPACE");
    require(test_srcdir != nullptr);
    require(test_workspace != nullptr);
    return std::filesystem::path(test_srcdir) / test_workspace;
}

std::vector<std::uint8_t> read_binary_file(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    require(stream.good());
    return std::vector<std::uint8_t>(
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>());
}

std::uint32_t read_u32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    std::uint32_t value = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        value |= static_cast<std::uint32_t>(bytes[offset++]) << shift;
    }
    return value;
}

std::uint64_t read_u64(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    std::uint64_t value = 0;
    for (int shift = 0; shift < 64; shift += 8) {
        value |= static_cast<std::uint64_t>(bytes[offset++]) << shift;
    }
    return value;
}

float read_f32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return std::bit_cast<float>(read_u32(bytes, offset));
}

// The 88-byte wrapper mesh_bake.cc::wrap_artifact writes ahead of the payload.
struct NavArtifactHeader {
    std::uint64_t payload_size = 0;
    std::uint32_t payload_endianness = 0;
    std::array<float, 3> bounds_min{};
    std::array<float, 3> bounds_max{};
};

bool parse_nav_artifact(
    const std::vector<std::uint8_t>& artifact,
    NavArtifactHeader* header) {
    if (artifact.size() < kMeshArtifactHeaderSize ||
        !std::equal(kRecastMagic.begin(), kRecastMagic.end(), artifact.begin())) {
        return false;
    }
    header->payload_endianness = read_u32(artifact, 24);
    header->payload_size = read_u64(artifact, 56);
    for (std::size_t axis = 0; axis < 3; ++axis) {
        header->bounds_min[axis] = read_f32(artifact, 64 + axis * 4);
        header->bounds_max[axis] = read_f32(artifact, 76 + axis * 4);
    }
    return artifact.size() >= kMeshArtifactHeaderSize + header->payload_size;
}

// Owns the pair, because dtNavMeshQuery holds a raw pointer into dtNavMesh and
// the mesh takes ownership of the payload allocation via DT_TILE_FREE_DATA.
class NavRuntime {
public:
    NavRuntime() = default;
    ~NavRuntime() {
        if (query_ != nullptr) {
            dtFreeNavMeshQuery(query_);
        }
        if (mesh_ != nullptr) {
            dtFreeNavMesh(mesh_);
        }
    }
    NavRuntime(const NavRuntime&) = delete;
    NavRuntime& operator=(const NavRuntime&) = delete;

    // Mirrors mesh_bake.cc::initialize_detour_navmesh, which does exactly this
    // and then throws the result away because it only wanted the validation.
    bool load(
        const std::vector<std::uint8_t>& artifact,
        const NavArtifactHeader& header,
        std::string* error) {
        auto* nav_data = static_cast<unsigned char*>(
            dtAlloc(static_cast<int>(header.payload_size), DT_ALLOC_PERM));
        if (nav_data == nullptr) {
            *error = "navmesh allocation failed";
            return false;
        }
        std::memcpy(
            nav_data,
            artifact.data() + kMeshArtifactHeaderSize,
            static_cast<std::size_t>(header.payload_size));
        if (header.payload_endianness != kLittleEndian) {
            if (!dtNavMeshHeaderSwapEndian(
                    nav_data, static_cast<int>(header.payload_size)) ||
                !dtNavMeshDataSwapEndian(
                    nav_data, static_cast<int>(header.payload_size))) {
                dtFree(nav_data);
                *error = "navmesh endian conversion failed";
                return false;
            }
        }
        mesh_ = dtAllocNavMesh();
        if (mesh_ == nullptr ||
            dtStatusFailed(mesh_->init(
                nav_data,
                static_cast<int>(header.payload_size),
                DT_TILE_FREE_DATA))) {
            dtFree(nav_data);
            *error = "navmesh init failed";
            return false;
        }
        query_ = dtAllocNavMeshQuery();
        if (query_ == nullptr ||
            dtStatusFailed(query_->init(mesh_, kMaxPathPolys))) {
            *error = "navmesh query init failed";
            return false;
        }
        return true;
    }

    const dtNavMesh& mesh() const { return *mesh_; }
    const dtNavMeshQuery& query() const { return *query_; }

private:
    dtNavMesh* mesh_ = nullptr;
    dtNavMeshQuery* query_ = nullptr;
};

std::uint64_t next_random(std::uint64_t* state) {
    *state += 0x9e3779b97f4a7c15ull;
    std::uint64_t value = *state;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
    return value ^ (value >> 31);
}

float next_unit(std::uint64_t* state) {
    return static_cast<float>(
        static_cast<double>(next_random(state) >> 40) * (1.0 / 16777216.0));
}

float horizontal_distance(const float* from, const float* to) {
    const float dx = to[0] - from[0];
    const float dz = to[2] - from[2];
    return std::sqrt(dx * dx + dz * dz);
}

struct SampledPoint {
    dtPolyRef ref = 0;
    std::array<float, 3> position{};
};

struct Bucket {
    const char* label;
    float min_meters;
    float max_meters;
    std::size_t pairs = 0;
    std::size_t connected = 0;
    std::size_t straight_walkable = 0;
    std::size_t path_buffer_full = 0;
    // Only for connected pairs whose straight line left the navmesh.
    std::vector<float> blocked_detour_ratios;
};

float percentile(std::vector<float>* values, float fraction) {
    if (values->empty()) {
        return 0.0f;
    }
    std::sort(values->begin(), values->end());
    const std::size_t index = std::min(
        values->size() - 1,
        static_cast<std::size_t>(fraction * static_cast<float>(values->size())));
    return (*values)[index];
}

float percent(std::size_t part, std::size_t whole) {
    if (whole == 0) {
        return 0.0f;
    }
    return 100.0f * static_cast<float>(part) / static_cast<float>(whole);
}

void measure(const std::vector<std::uint8_t>& artifact, const std::string& name) {
    NavArtifactHeader header;
    if (!parse_nav_artifact(artifact, &header)) {
        std::printf("\nnavmesh %s: artifact header rejected\n", name.c_str());
        return;
    }

    NavRuntime nav;
    std::string error;
    if (!nav.load(artifact, header, &error)) {
        std::printf("\nnavmesh %s: %s\n", name.c_str(), error.c_str());
        return;
    }

    int tile_count = 0;
    int poly_count = 0;
    for (int index = 0; index < nav.mesh().getMaxTiles(); ++index) {
        const dtMeshTile* tile = nav.mesh().getTile(index);
        if (tile == nullptr || tile->header == nullptr) {
            continue;
        }
        ++tile_count;
        poly_count += tile->header->polyCount;
    }

    std::printf(
        "\nnavmesh %-16s payload %llu B  tiles %d  polys %d\n",
        name.c_str(),
        static_cast<unsigned long long>(header.payload_size),
        tile_count,
        poly_count);
    std::printf(
        "  bounds x[%.1f, %.1f]  y[%.1f, %.1f]  z[%.1f, %.1f]\n",
        header.bounds_min[0], header.bounds_max[0],
        header.bounds_min[1], header.bounds_max[1],
        header.bounds_min[2], header.bounds_max[2]);

    const float mid_y = (header.bounds_min[1] + header.bounds_max[1]) * 0.5f;
    const float half_extent_y =
        (header.bounds_max[1] - header.bounds_min[1]) * 0.5f + 2.0f;
    const float half_extents[3] = {
        kSnapHalfExtentMeters,
        half_extent_y,
        kSnapHalfExtentMeters,
    };
    const float exact_half_extents[3] = {
        kExactHalfExtentMeters,
        half_extent_y,
        kExactHalfExtentMeters,
    };
    const dtQueryFilter filter;

    // Seeded so two runs over the same terrain are comparable, and so a
    // surprising number can be re-examined at the same sample.
    std::uint64_t random_state = 0x5eed'0000'0000'1234ull;
    std::vector<SampledPoint> walkable;
    walkable.reserve(kSamplePoints);
    std::size_t exact_hits = 0;
    for (std::size_t index = 0; index < kSamplePoints; ++index) {
        const float candidate[3] = {
            header.bounds_min[0] +
                next_unit(&random_state) *
                    (header.bounds_max[0] - header.bounds_min[0]),
            mid_y,
            header.bounds_min[2] +
                next_unit(&random_state) *
                    (header.bounds_max[2] - header.bounds_min[2]),
        };
        SampledPoint exact;
        if (dtStatusSucceed(nav.query().findNearestPoly(
                candidate,
                exact_half_extents,
                &filter,
                &exact.ref,
                exact.position.data())) &&
            exact.ref != 0) {
            ++exact_hits;
        }
        SampledPoint point;
        if (dtStatusFailed(nav.query().findNearestPoly(
                candidate,
                half_extents,
                &filter,
                &point.ref,
                point.position.data())) ||
            point.ref == 0) {
            continue;
        }
        walkable.push_back(point);
    }

    std::printf(
        "  on-navmesh exact    %5zu / %zu points   %5.1f%%   "
        "(already walkable, within %.1f m)\n",
        exact_hits,
        kSamplePoints,
        percent(exact_hits, kSamplePoints),
        kExactHalfExtentMeters);
    std::printf(
        "  on-navmesh snapped  %5zu / %zu points   %5.1f%%   "
        "(pullable onto the navmesh, within %.1f m)\n",
        walkable.size(),
        kSamplePoints,
        percent(walkable.size(), kSamplePoints),
        kSnapHalfExtentMeters);

    if (walkable.size() < 2) {
        std::printf("  too few walkable points to route between\n");
        return;
    }

    std::vector<Bucket> buckets = {
        {"0-20 m", 0.0f, 20.0f, 0, 0, 0, 0, {}},
        {"20-50 m", 20.0f, 50.0f, 0, 0, 0, 0, {}},
        {"50-100 m", 50.0f, 100.0f, 0, 0, 0, 0, {}},
        {"100+ m", 100.0f, 1e9f, 0, 0, 0, 0, {}},
        {"all", 0.0f, 1e9f, 0, 0, 0, 0, {}},
    };

    std::array<dtPolyRef, kMaxPathPolys> path{};
    std::array<float, kMaxStraightPathPoints * 3> straight_path{};
    std::array<dtPolyRef, kMaxPathPolys> ray_path{};

    std::size_t straight_path_overflow = 0;
    for (std::size_t pair = 0; pair < kRoutePairs; ++pair) {
        const SampledPoint& start =
            walkable[next_random(&random_state) % walkable.size()];
        const SampledPoint& end =
            walkable[next_random(&random_state) % walkable.size()];
        if (start.ref == end.ref) {
            continue;
        }
        const float line_meters =
            horizontal_distance(start.position.data(), end.position.data());
        if (line_meters <= 0.0f) {
            continue;
        }

        int path_count = 0;
        const dtStatus path_status = nav.query().findPath(
            start.ref,
            end.ref,
            start.position.data(),
            end.position.data(),
            &filter,
            path.data(),
            &path_count,
            kMaxPathPolys);
        const bool buffer_full =
            dtStatusDetail(path_status, DT_BUFFER_TOO_SMALL) != 0;
        // A partial result comes back as a valid status with a corridor that
        // stops short, so the end polygon has to be checked explicitly.
        const bool connected = dtStatusSucceed(path_status) && path_count > 0 &&
            path[path_count - 1] == end.ref;

        float path_meters = 0.0f;
        bool have_length = false;
        if (connected) {
            int straight_count = 0;
            const dtStatus straight_status = nav.query().findStraightPath(
                start.position.data(),
                end.position.data(),
                path.data(),
                path_count,
                straight_path.data(),
                nullptr,
                nullptr,
                &straight_count,
                kMaxStraightPathPoints);
            if (dtStatusSucceed(straight_status) && straight_count >= 2) {
                for (int corner = 1; corner < straight_count; ++corner) {
                    path_meters += horizontal_distance(
                        &straight_path[(corner - 1) * 3],
                        &straight_path[corner * 3]);
                }
                have_length = true;
            } else {
                ++straight_path_overflow;
            }
        }

        // Detour's surface raycast walks the navmesh along the segment and
        // reports where it leaves it. t is FLT_MAX when it never does, which is
        // exactly "this chord is walkable as authored".
        float hit_fraction = 0.0f;
        float hit_normal[3]{};
        int ray_path_count = 0;
        const dtStatus ray_status = nav.query().raycast(
            start.ref,
            start.position.data(),
            end.position.data(),
            &filter,
            &hit_fraction,
            hit_normal,
            ray_path.data(),
            &ray_path_count,
            kMaxPathPolys);
        const bool straight_walkable =
            dtStatusSucceed(ray_status) && hit_fraction >= 1.0f;

        for (Bucket& bucket : buckets) {
            if (line_meters < bucket.min_meters ||
                line_meters >= bucket.max_meters) {
                continue;
            }
            ++bucket.pairs;
            bucket.connected += connected ? 1 : 0;
            bucket.straight_walkable += straight_walkable ? 1 : 0;
            bucket.path_buffer_full += buffer_full ? 1 : 0;
            if (!straight_walkable && have_length && path_meters > 0.0f) {
                bucket.blocked_detour_ratios.push_back(path_meters / line_meters);
            }
        }
    }

    std::printf(
        "  %-10s %7s %11s %11s %8s %8s %8s %8s\n",
        "bucket", "pairs", "connected", "straight", "blocked",
        "detour", "detour", "detour");
    std::printf(
        "  %-10s %7s %11s %11s %8s %8s %8s %8s\n",
        "", "", "", "walkable", "pairs", "p50", "p90", "max");
    for (Bucket& bucket : buckets) {
        const float p50 = percentile(&bucket.blocked_detour_ratios, 0.50f);
        const float p90 = percentile(&bucket.blocked_detour_ratios, 0.90f);
        const float worst = bucket.blocked_detour_ratios.empty()
            ? 0.0f
            : bucket.blocked_detour_ratios.back();
        std::printf(
            "  %-10s %7zu %10.1f%% %10.1f%% %8zu %8.2f %8.2f %8.2f\n",
            bucket.label,
            bucket.pairs,
            percent(bucket.connected, bucket.pairs),
            percent(bucket.straight_walkable, bucket.pairs),
            bucket.blocked_detour_ratios.size(),
            p50,
            p90,
            worst);
        if (bucket.path_buffer_full > 0) {
            std::printf(
                "  %-10s   path buffer full on %zu pairs -- raise kMaxPathPolys\n",
                "",
                bucket.path_buffer_full);
        }
    }
    if (straight_path_overflow > 0) {
        std::printf(
            "  corner buffer full on %zu connected pairs -- "
            "raise kMaxStraightPathPoints\n",
            straight_path_overflow);
    }
}

// Shipping terrain arrives through the catalog bundle, which is the path
// game_server would really use.
void measure_bundled(const std::vector<std::uint8_t>& bundle, const std::string& name) {
    try {
        measure(
            network_example::game_server::load_gameplay_bundle_entry_bytes(
                bundle.data(),
                static_cast<std::uint32_t>(bundle.size()),
                "mesh_assets/recast/" + name + ".navmesh"),
            name);
    } catch (const std::exception& error) {
        std::printf("\nnavmesh %s: not in bundle (%s)\n", name.c_str(), error.what());
    }
}

// Measurement-only terrain is baked into its own package and read straight from
// runfiles, so that nothing built to be measured ends up in a client download.
void measure_test_asset(const std::string& name) {
    measure(
        read_binary_file((runfiles_root() / "game_server" / "test_mesh_assets" /
                          "generated" / "recast" / (name + ".navmesh"))
                             .string()),
        name);
}

}  // namespace

int main() {
    const std::vector<std::uint8_t> bundle = read_binary_file(
        (runfiles_root() / "game_server" / "gameplay_catalog_bundle" / "bundle.zip")
            .string());

    std::printf(
        "patrol route feasibility -- %zu sample points, %zu route pairs per "
        "terrain\n",
        kSamplePoints,
        kRoutePairs);

    // plane_200x200 is what the shipping catalog names, and it is flat: every
    // number it produces is trivially perfect. It is measured anyway, as the
    // control that says the harness is reading a real navmesh.
    measure_bundled(bundle, "plane_200x200");
    // Rolling ground: answers what height alone does to a straight route.
    measure_bundled(bundle, "undulating");
    // Flat ground with pits and walls: answers what a built level does to
    // placement coverage and to connectivity, which a gapless heightfield
    // cannot.
    measure_test_asset("obstructed_field");
    return 0;
}
