#include "game_server/src/terrain/terrain_assets.h"

#include <cstring>

namespace network_example::game_server::terrain {
namespace {

constexpr std::size_t kHeaderSize = 24;
constexpr std::uint32_t kMaxSamples = 16384;  // per axis; 4.9 km at 0.3 m

std::uint16_t le16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

std::uint32_t le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
        (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

void put16(std::vector<std::uint8_t>& b, std::uint16_t v) {
    b.push_back(static_cast<std::uint8_t>(v));
    b.push_back(static_cast<std::uint8_t>(v >> 8));
}

void put32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    for (int k = 0; k < 4; ++k) {
        b.push_back(static_cast<std::uint8_t>(v >> (8 * k)));
    }
}

struct Header {
    std::uint16_t flags = 0;
    std::uint32_t a = 0;  // cells_x (NKHF) or samples_x (NKGM)
    std::uint32_t b = 0;
    std::uint32_t spacing_mm = 0;
};

bool read_header(std::span<const std::uint8_t> bytes, const char* magic, Header* h, std::string* error) {
    if (bytes.size() < kHeaderSize || std::memcmp(bytes.data(), magic, 4) != 0) {
        *error = std::string("not an ") + magic + " file";
        return false;
    }
    const std::uint8_t* p = bytes.data();
    if (le16(p + 4) != 1) {
        *error = std::string(magic) + " version is not 1";
        return false;
    }
    h->flags = le16(p + 6);
    h->a = le32(p + 8);
    h->b = le32(p + 12);
    h->spacing_mm = le32(p + 16);
    if (le32(p + 20) != 0) {
        *error = std::string(magic) + " reserved field is not 0";
        return false;
    }
    return true;
}

}  // namespace

bool parse_height_grid(std::span<const std::uint8_t> bytes, HeightGrid* out, std::string* error) {
    Header h;
    if (!read_header(bytes, "NKHF", &h, error)) {
        return false;
    }
    if (h.spacing_mm != static_cast<std::uint32_t>(kCellMm)) {
        *error = "NKHF spacing is not 300 mm";
        return false;
    }
    if (h.a == 0 || h.b == 0 || h.a >= kMaxSamples || h.b >= kMaxSamples) {
        *error = "NKHF size out of range";
        return false;
    }
    const std::uint64_t nx = h.a + 1u, nz = h.b + 1u;
    if (bytes.size() != kHeaderSize + 2u * nx * nz) {
        *error = "NKHF size does not match its header";
        return false;
    }
    out->samples_x = static_cast<std::int32_t>(nx);
    out->samples_z = static_cast<std::int32_t>(nz);
    out->cm.resize(static_cast<std::size_t>(nx * nz));
    for (std::size_t k = 0; k < out->cm.size(); ++k) {
        out->cm[k] = static_cast<std::int16_t>(le16(bytes.data() + kHeaderSize + 2 * k));
    }
    return true;
}

bool parse_stamp(std::span<const std::uint8_t> bytes, std::string name, StampAsset* out, std::string* error) {
    HeightGrid grid;
    if (!parse_height_grid(bytes, &grid, error)) {
        return false;
    }
    if ((le16(bytes.data() + 6) & 1u) == 0) {
        *error = "NKHF is not a stamp (flags bit 0 clear)";
        return false;
    }
    const std::int32_t cx = grid.samples_x - 1, cz = grid.samples_z - 1;
    if (cx % kCellsPerGrid != 0 || cz % kCellsPerGrid != 0) {
        *error = "stamp edges are not a multiple of 12 m";
        return false;
    }
    for (std::int32_t j = 0; j <= cz; ++j) {
        for (std::int32_t i = 0; i <= cx; ++i) {
            if ((i == 0 || j == 0 || i == cx || j == cz) && grid.at(i, j) != 0) {
                *error = "stamp border sample is not on the water line";
                return false;
            }
        }
    }
    out->name = std::move(name);
    out->cells_x = cx;
    out->cells_z = cz;
    out->cm = std::move(grid.cm);
    return true;
}

std::vector<std::uint8_t> write_nkhf(const HeightGrid& grid, std::uint16_t flags) {
    std::vector<std::uint8_t> b;
    b.reserve(kHeaderSize + 2 * grid.cm.size());
    b.insert(b.end(), {'N', 'K', 'H', 'F'});
    put16(b, 1);
    put16(b, flags);
    put32(b, static_cast<std::uint32_t>(grid.samples_x - 1));
    put32(b, static_cast<std::uint32_t>(grid.samples_z - 1));
    put32(b, static_cast<std::uint32_t>(kCellMm));
    put32(b, 0);
    for (const std::int16_t v : grid.cm) {
        put16(b, static_cast<std::uint16_t>(v));
    }
    return b;
}

bool parse_ground_map(std::span<const std::uint8_t> bytes, std::string name, GroundMapAsset* out, std::string* error) {
    Header h;
    if (!read_header(bytes, "NKGM", &h, error)) {
        return false;
    }
    if (h.spacing_mm != static_cast<std::uint32_t>(kGroundMapSpacingMm)) {
        *error = "NKGM spacing is not 1200 mm";
        return false;
    }
    if (h.a < 2 || h.b < 2 || h.a > kMaxSamples || h.b > kMaxSamples) {
        *error = "NKGM size out of range";
        return false;
    }
    if ((h.a - 1) % (kGridMm / kGroundMapSpacingMm) != 0 || (h.b - 1) % (kGridMm / kGroundMapSpacingMm) != 0) {
        *error = "NKGM edges are not a multiple of 12 m";
        return false;
    }
    const std::uint64_t n = static_cast<std::uint64_t>(h.a) * h.b;
    if (bytes.size() != kHeaderSize + 2u * n) {
        *error = "NKGM size does not match its header";
        return false;
    }
    out->name = std::move(name);
    out->samples_x = static_cast<std::int32_t>(h.a);
    out->samples_z = static_cast<std::int32_t>(h.b);
    out->gray.resize(static_cast<std::size_t>(n));
    for (std::size_t k = 0; k < out->gray.size(); ++k) {
        out->gray[k] = le16(bytes.data() + kHeaderSize + 2 * k);
    }
    return true;
}

std::vector<std::uint8_t> write_nkgm(const GroundMapAsset& map) {
    std::vector<std::uint8_t> b;
    b.reserve(kHeaderSize + 2 * map.gray.size());
    b.insert(b.end(), {'N', 'K', 'G', 'M'});
    put16(b, 1);
    put16(b, 0);
    put32(b, static_cast<std::uint32_t>(map.samples_x));
    put32(b, static_cast<std::uint32_t>(map.samples_z));
    put32(b, static_cast<std::uint32_t>(kGroundMapSpacingMm));
    put32(b, 0);
    for (const std::uint16_t v : map.gray) {
        put16(b, v);
    }
    return b;
}

}  // namespace network_example::game_server::terrain
