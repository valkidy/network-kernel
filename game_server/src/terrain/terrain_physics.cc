#include "game_server/src/terrain/terrain_physics.h"

#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/StreamWrapper.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <sstream>

namespace network_example::game_server::terrain {
namespace {

// physics_world.cc's artifact header (kJoltMagic, schema 1, 88 bytes).
constexpr std::array<std::uint8_t, 8> kJoltMagic = {'N', 'K', 'M', 'J', 'O', 'L', 'T', '1'};
constexpr std::uint32_t kArtifactSchemaVersion = 1;
constexpr std::uint32_t kArtifactHeaderSize = 88;

void put32(std::vector<std::uint8_t>* b, std::uint32_t v) {
    for (int k = 0; k < 4; ++k) b->push_back(static_cast<std::uint8_t>(v >> (8 * k)));
}
void put64(std::vector<std::uint8_t>* b, std::uint64_t v) {
    for (int k = 0; k < 8; ++k) b->push_back(static_cast<std::uint8_t>(v >> (8 * k)));
}
void putf(std::vector<std::uint8_t>* b, float v) { put32(b, std::bit_cast<std::uint32_t>(v)); }

std::uint64_t jolt_version_id() {
    using JPH::uint64;
    return JPH_VERSION_ID;
}

std::uint32_t native_endianness() {
    return std::endian::native == std::endian::little ? 1u : 2u;
}

// Jolt's Factory is process-global. Create one only when nobody has (the
// kernel's PhysicsWorld refuses to start while a Factory it does not own
// exists), and tear it down again afterwards -- mesh_bake's JoltRuntime.
class ScopedJoltRuntime final {
public:
    ScopedJoltRuntime() {
        JPH::RegisterDefaultAllocator();
        if (JPH::Factory::sInstance != nullptr) {
            return;
        }
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
        owned_ = true;
    }
    ~ScopedJoltRuntime() {
        if (!owned_) return;
        JPH::UnregisterTypes();
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
    }
    ScopedJoltRuntime(const ScopedJoltRuntime&) = delete;
    ScopedJoltRuntime& operator=(const ScopedJoltRuntime&) = delete;

private:
    bool owned_ = false;
};

}  // namespace

float terrain_world_origin_x(const HeightGrid& grid) {
    return -static_cast<float>(grid.samples_x - 1) * static_cast<float>(kCellMm) / 2000.0f;
}

float terrain_world_origin_z(const HeightGrid& grid) {
    return -static_cast<float>(grid.samples_z - 1) * static_cast<float>(kCellMm) / 2000.0f;
}

bool build_static_scene_artifact(const HeightGrid& grid, const TerrainArtifactOptions& options,
    std::vector<std::uint8_t>* artifact, TerrainArtifactStats* stats, std::string* error) {
    if (grid.samples_x < 2 || grid.samples_z < 2 ||
        grid.cm.size() != static_cast<std::size_t>(grid.samples_x) * static_cast<std::size_t>(grid.samples_z)) {
        *error = "terrain heightfield is empty or malformed";
        return false;
    }
    if (options.tile_cells < 2 || options.block_size < 2 || options.block_size > 8 ||
        options.bits_per_sample < 1 || options.bits_per_sample > 16) {
        *error = "terrain artifact options out of range";
        return false;
    }
    ScopedJoltRuntime runtime;
    const float cell = static_cast<float>(kCellMm) / 1000.0f;
    const float ox = terrain_world_origin_x(grid), oz = terrain_world_origin_z(grid);
    const std::int32_t cx = grid.samples_x - 1, cz = grid.samples_z - 1;
    JPH::StaticCompoundShapeSettings compound;
    float lo = 1e30f, hi = -1e30f;
    std::uint32_t tiles = 0;
    for (std::int32_t tz = 0; tz < cz; tz += options.tile_cells) {
        for (std::int32_t tx = 0; tx < cx; tx += options.tile_cells) {
            // A square tile; past the map edge its samples are no-collision.
            const std::int32_t n = options.tile_cells + 1;
            JPH::Array<float> samples(static_cast<std::size_t>(n) * static_cast<std::size_t>(n), JPH::HeightFieldShapeConstants::cNoCollisionValue);
            for (std::int32_t j = 0; j < n && tz + j <= cz; ++j) {
                for (std::int32_t i = 0; i < n && tx + i <= cx; ++i) {
                    const float h = static_cast<float>(grid.at(tx + i, tz + j)) / 100.0f;
                    samples[static_cast<std::size_t>(j) * static_cast<std::size_t>(n) + static_cast<std::size_t>(i)] = h;
                    lo = std::min(lo, h);
                    hi = std::max(hi, h);
                }
            }
            JPH::HeightFieldShapeSettings settings(samples.data(), JPH::Vec3::sZero(), JPH::Vec3(cell, 1.0f, cell), static_cast<JPH::uint32>(n));
            settings.mBlockSize = options.block_size;
            settings.mBitsPerSample = options.bits_per_sample;
            JPH::ShapeSettings::ShapeResult tile = settings.Create();
            if (tile.HasError()) {
                *error = "Jolt HeightFieldShape creation failed: " + std::string(tile.GetError().c_str());
                return false;
            }
            compound.AddShape(JPH::Vec3(ox + static_cast<float>(tx) * cell, 0.0f, oz + static_cast<float>(tz) * cell), JPH::Quat::sIdentity(), tile.Get());
            ++tiles;
        }
    }
    JPH::ShapeSettings::ShapeResult shape = compound.Create();
    if (shape.HasError()) {
        *error = "Jolt StaticCompoundShape creation failed: " + std::string(shape.GetError().c_str());
        return false;
    }
    std::ostringstream output(std::ios::binary | std::ios::out);
    JPH::StreamOutWrapper stream(output);
    JPH::Shape::ShapeToIDMap shape_map;
    JPH::Shape::MaterialToIDMap material_map;
    shape.Get()->SaveWithChildren(stream, shape_map, material_map);
    if (stream.IsFailed()) {
        *error = "Jolt terrain shape serialization failed";
        return false;
    }
    const std::string payload = output.str();

    artifact->clear();
    artifact->reserve(kArtifactHeaderSize + payload.size());
    artifact->insert(artifact->end(), kJoltMagic.begin(), kJoltMagic.end());
    put32(artifact, kArtifactSchemaVersion);
    put32(artifact, kArtifactHeaderSize);
    put64(artifact, jolt_version_id());
    put32(artifact, native_endianness());
    put32(artifact, static_cast<std::uint32_t>(grid.cm.size()));        // vertex_count: samples
    put32(artifact, static_cast<std::uint32_t>(2u * static_cast<std::uint32_t>(cx) * static_cast<std::uint32_t>(cz)));
    put32(artifact, 0);
    put64(artifact, height_grid_hash(grid));                             // source_hash
    put64(artifact, (static_cast<std::uint64_t>(options.tile_cells) << 16) | (options.block_size << 8) | options.bits_per_sample);
    put64(artifact, payload.size());
    putf(artifact, ox); putf(artifact, lo); putf(artifact, oz);
    putf(artifact, -ox); putf(artifact, hi); putf(artifact, -oz);
    artifact->insert(artifact->end(), payload.begin(), payload.end());
    if (stats != nullptr) {
        stats->tiles = tiles;
        stats->payload_bytes = payload.size();
    }
    return true;
}

}  // namespace network_example::game_server::terrain
