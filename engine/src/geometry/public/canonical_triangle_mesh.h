#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace network_example {

struct CanonicalMeshBounds {
    std::array<float, 3> min{};
    std::array<float, 3> max{};
};

struct CanonicalTriangleMesh {
    std::vector<std::array<float, 3>> positions;
    std::vector<std::uint32_t> triangle_indices;
    CanonicalMeshBounds bounds;
};

}  // namespace network_example
