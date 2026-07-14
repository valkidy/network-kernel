#include "geometry/public/obj_importer.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace network_example {
namespace {

ObjImportResult failure(std::size_t line_number, const std::string& message) {
    ObjImportResult result;
    result.error = "OBJ line " + std::to_string(line_number) + ": " + message;
    return result;
}

bool parse_face_index(
    const std::string& token,
    std::size_t vertex_count,
    std::uint32_t* index) {
    const std::string_view token_view(token);
    const std::string_view vertex_part = token_view.substr(0, token_view.find('/'));
    if (vertex_part.empty()) {
        return false;
    }

    std::int64_t obj_index = 0;
    const char* begin = vertex_part.data();
    const char* end = begin + vertex_part.size();
    const auto [parsed_end, error] = std::from_chars(begin, end, obj_index);
    if (error != std::errc{} || parsed_end != end || obj_index == 0) {
        return false;
    }

    const std::int64_t resolved = obj_index > 0
        ? obj_index - 1
        : static_cast<std::int64_t>(vertex_count) + obj_index;
    if (resolved < 0 || resolved >= static_cast<std::int64_t>(vertex_count) ||
        resolved > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    *index = static_cast<std::uint32_t>(resolved);
    return true;
}

bool is_degenerate(
    const std::array<float, 3>& a,
    const std::array<float, 3>& b,
    const std::array<float, 3>& c) {
    const double ab_x = static_cast<double>(b[0]) - a[0];
    const double ab_y = static_cast<double>(b[1]) - a[1];
    const double ab_z = static_cast<double>(b[2]) - a[2];
    const double ac_x = static_cast<double>(c[0]) - a[0];
    const double ac_y = static_cast<double>(c[1]) - a[1];
    const double ac_z = static_cast<double>(c[2]) - a[2];
    const double cross_x = ab_y * ac_z - ab_z * ac_y;
    const double cross_y = ab_z * ac_x - ab_x * ac_z;
    const double cross_z = ab_x * ac_y - ab_y * ac_x;
    return cross_x * cross_x + cross_y * cross_y + cross_z * cross_z <= 1e-20;
}

ObjImportResult validate(CanonicalTriangleMesh mesh) {
    if (mesh.positions.empty()) {
        return failure(0, "mesh has no vertices");
    }
    if (mesh.triangle_indices.empty() || mesh.triangle_indices.size() % 3 != 0) {
        return failure(0, "mesh has no complete triangles");
    }

    mesh.bounds.min = mesh.positions.front();
    mesh.bounds.max = mesh.positions.front();
    for (const auto& position : mesh.positions) {
        for (std::size_t axis = 0; axis < position.size(); ++axis) {
            if (!std::isfinite(position[axis])) {
                return failure(0, "mesh has a non-finite vertex");
            }
            mesh.bounds.min[axis] = std::min(mesh.bounds.min[axis], position[axis]);
            mesh.bounds.max[axis] = std::max(mesh.bounds.max[axis], position[axis]);
        }
    }

    for (std::size_t i = 0; i < mesh.triangle_indices.size(); i += 3) {
        const std::uint32_t a = mesh.triangle_indices[i];
        const std::uint32_t b = mesh.triangle_indices[i + 1];
        const std::uint32_t c = mesh.triangle_indices[i + 2];
        if (a >= mesh.positions.size() || b >= mesh.positions.size() ||
            c >= mesh.positions.size()) {
            return failure(0, "triangle index is out of range");
        }
        if (is_degenerate(mesh.positions[a], mesh.positions[b], mesh.positions[c])) {
            return failure(0, "mesh has a degenerate triangle");
        }
    }

    ObjImportResult result;
    result.mesh = std::move(mesh);
    return result;
}

}  // namespace

ObjImportResult import_obj(std::istream& input) {
    try {
        CanonicalTriangleMesh mesh;
        std::string line;
        std::size_t line_number = 0;
        while (std::getline(input, line)) {
            ++line_number;
            std::istringstream row(line);
            std::string type;
            row >> type;
            if (type.empty() || type[0] == '#') {
                continue;
            }
            if (type == "v") {
                std::array<float, 3> position{};
                if (!(row >> position[0] >> position[1] >> position[2])) {
                    return failure(line_number, "invalid vertex");
                }
                mesh.positions.push_back(position);
                continue;
            }
            if (type == "f") {
                std::vector<std::uint32_t> face;
                std::string token;
                while (row >> token) {
                    if (!token.empty() && token[0] == '#') {
                        break;
                    }
                    std::uint32_t index = 0;
                    if (!parse_face_index(token, mesh.positions.size(), &index)) {
                        return failure(line_number, "invalid face index");
                    }
                    face.push_back(index);
                }
                if (face.size() < 3) {
                    return failure(line_number, "face has fewer than three vertices");
                }
                for (std::size_t i = 2; i < face.size(); ++i) {
                    mesh.triangle_indices.push_back(face[0]);
                    mesh.triangle_indices.push_back(face[i - 1]);
                    mesh.triangle_indices.push_back(face[i]);
                }
            }
        }
        if (input.bad()) {
            return failure(line_number, "input read failed");
        }
        return validate(std::move(mesh));
    } catch (const std::exception& exception) {
        ObjImportResult result;
        result.error = std::string("OBJ import failed: ") + exception.what();
        return result;
    }
}

ObjImportResult import_obj_file(const std::string& path) {
    std::ifstream input(path);
    if (!input) {
        ObjImportResult result;
        result.error = "cannot open OBJ: " + path;
        return result;
    }
    return import_obj(input);
}

}  // namespace network_example
