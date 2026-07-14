#pragma once

#include <istream>
#include <string>

#include "geometry/public/canonical_triangle_mesh.h"

namespace network_example {

struct ObjImportResult {
    CanonicalTriangleMesh mesh;
    std::string error;

    explicit operator bool() const { return error.empty(); }
};

ObjImportResult import_obj(std::istream& input);
ObjImportResult import_obj_file(const std::string& path);

// TODO(phase2): Evaluate GLB support without changing the Phase 1 OBJ contract.

}  // namespace network_example
