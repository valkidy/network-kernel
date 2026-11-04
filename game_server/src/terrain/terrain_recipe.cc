#include "game_server/src/terrain/terrain_recipe.h"

#include <cstring>

namespace network_example::game_server::terrain {
namespace {

class Writer {
public:
    std::vector<std::uint8_t> bytes;
    void u8(std::uint8_t v) { bytes.push_back(v); }
    void u16(std::uint16_t v) { for (int k = 0; k < 2; ++k) bytes.push_back(static_cast<std::uint8_t>(v >> (8 * k))); }
    void u32(std::uint32_t v) { for (int k = 0; k < 4; ++k) bytes.push_back(static_cast<std::uint8_t>(v >> (8 * k))); }
    void u64(std::uint64_t v) { for (int k = 0; k < 8; ++k) bytes.push_back(static_cast<std::uint8_t>(v >> (8 * k))); }
    void i32(std::int32_t v) { u32(static_cast<std::uint32_t>(v)); }
    void i64(std::int64_t v) { u64(static_cast<std::uint64_t>(v)); }
};

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> b) : b_(b) {}
    bool ok() const { return ok_; }
    bool done() const { return p_ == b_.size(); }
    std::uint64_t get(int n) {
        if (p_ + static_cast<std::size_t>(n) > b_.size()) { ok_ = false; return 0; }
        std::uint64_t v = 0;
        for (int k = 0; k < n; ++k) v |= static_cast<std::uint64_t>(b_[p_++]) << (8 * k);
        return v;
    }
    std::uint8_t u8() { return static_cast<std::uint8_t>(get(1)); }
    std::uint16_t u16() { return static_cast<std::uint16_t>(get(2)); }
    std::uint32_t u32() { return static_cast<std::uint32_t>(get(4)); }
    std::uint64_t u64() { return get(8); }
    std::int32_t i32() { return static_cast<std::int32_t>(u32()); }
    std::int64_t i64() { return static_cast<std::int64_t>(u64()); }

private:
    std::span<const std::uint8_t> b_;
    std::size_t p_ = 0;
    bool ok_ = true;
};

constexpr std::uint32_t kMaxPlacements = 1u << 16;

}  // namespace

std::uint64_t terrain_library_hash(const TerrainLibrary& lib) {
    Fnv64 h;
    h.bytes("NKTL", 4);
    h.u32(static_cast<std::uint32_t>(lib.stamps.size()));
    for (const StampAsset& s : lib.stamps) {
        h.u32(static_cast<std::uint32_t>(s.name.size()));
        h.bytes(s.name.data(), s.name.size());
        h.u32(static_cast<std::uint32_t>(s.cells_x));
        h.u32(static_cast<std::uint32_t>(s.cells_z));
        for (const std::int16_t v : s.cm) h.i16(v);
    }
    h.u32(static_cast<std::uint32_t>(lib.ground_maps.size()));
    for (const GroundMapAsset& g : lib.ground_maps) {
        h.u32(static_cast<std::uint32_t>(g.name.size()));
        h.bytes(g.name.data(), g.name.size());
        h.u32(static_cast<std::uint32_t>(g.samples_x));
        h.u32(static_cast<std::uint32_t>(g.samples_z));
        for (const std::uint16_t v : g.gray) h.i16(static_cast<std::int16_t>(v));
    }
    return h.value();
}

std::vector<std::uint8_t> write_recipe(const StoredRecipe& s) {
    const TerrainRecipe& r = s.recipe;
    Writer w;
    w.bytes.insert(w.bytes.end(), {'N', 'K', 'T', 'R'});
    w.u16(1);
    w.u16(0);
    w.u64(s.library_hash);
    w.u64(s.heights_hash);
    w.i32(r.seed);
    w.i32(r.cells_x);
    w.i32(r.cells_z);
    w.u32(r.ground_map);
    w.u8(r.ground_orientation);
    w.u8(0); w.u8(0); w.u8(0);
    w.i32(r.terrace.levels);
    w.i32(r.terrace.level_height_mm);
    w.i32(r.terrace.black_mm);
    w.i32(r.terrace.riser_run_mm);
    w.i32(r.terrace.cliff_width_mm);
    w.i64(r.terrace.cliff_share_q16);
    w.i32(r.terrace.cliff_wavelength_mm);
    w.i32(r.detail.amplitude_mm);
    w.i32(r.detail.wavelength_mm);
    w.i64(r.ring_tan_q16);
    w.u32(static_cast<std::uint32_t>(r.placements.size()));
    for (const TerrainPlacement& p : r.placements) {
        w.u32(p.stamp);
        w.i32(p.x_cells);
        w.i32(p.z_cells);
        w.i32(p.base_cm);
        w.i32(p.ring_mm);
        w.u8(p.turns);
        w.u8(p.mirror ? 1 : 0);
        w.u8(0); w.u8(0);
    }
    return w.bytes;
}

bool parse_recipe(std::span<const std::uint8_t> bytes, StoredRecipe* out, std::string* error) {
    if (bytes.size() < 8 || std::memcmp(bytes.data(), "NKTR", 4) != 0) {
        *error = "not a terrain recipe (NKTR)";
        return false;
    }
    Reader rd(bytes.subspan(4));
    if (rd.u16() != 1 || rd.u16() != 0) {
        *error = "terrain recipe version is not 1";
        return false;
    }
    StoredRecipe s;
    TerrainRecipe& r = s.recipe;
    s.library_hash = rd.u64();
    s.heights_hash = rd.u64();
    r.seed = rd.i32();
    r.cells_x = rd.i32();
    r.cells_z = rd.i32();
    r.ground_map = rd.u32();
    r.ground_orientation = rd.u8();
    rd.u8(); rd.u8(); rd.u8();
    r.terrace.levels = rd.i32();
    r.terrace.level_height_mm = rd.i32();
    r.terrace.black_mm = rd.i32();
    r.terrace.riser_run_mm = rd.i32();
    r.terrace.cliff_width_mm = rd.i32();
    r.terrace.cliff_share_q16 = rd.i64();
    r.terrace.cliff_wavelength_mm = rd.i32();
    r.detail.amplitude_mm = rd.i32();
    r.detail.wavelength_mm = rd.i32();
    r.ring_tan_q16 = rd.i64();
    const std::uint32_t n = rd.u32();
    if (!rd.ok() || n > kMaxPlacements) {
        *error = "terrain recipe is truncated or has too many placements";
        return false;
    }
    for (std::uint32_t k = 0; k < n; ++k) {
        TerrainPlacement p;
        p.stamp = rd.u32();
        p.x_cells = rd.i32();
        p.z_cells = rd.i32();
        p.base_cm = rd.i32();
        p.ring_mm = rd.i32();
        p.turns = rd.u8();
        p.mirror = rd.u8() != 0;
        rd.u8(); rd.u8();
        r.placements.push_back(p);
    }
    if (!rd.ok() || !rd.done()) {
        *error = "terrain recipe size does not match its contents";
        return false;
    }
    *out = std::move(s);
    return true;
}

}  // namespace network_example::game_server::terrain
