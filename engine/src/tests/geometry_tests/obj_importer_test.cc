#include <cassert>
#include <sstream>
#include <string>

#include "geometry/public/obj_importer.h"

namespace {

network_example::ObjImportResult import_text(const std::string& text) {
    std::istringstream input(text);
    return network_example::import_obj(input);
}

}  // namespace

int main() {
    const auto quad = import_text(
        "v -1 0 -1\n"
        "v -1 0 1\n"
        "v 1 0 1\n"
        "v 1 0 -1\n"
        "f 1/1/1 2/2/1 3/3/1 4/4/1\n");
    assert(quad);
    assert(quad.mesh.positions.size() == 4);
    assert(quad.mesh.triangle_indices.size() == 6);
    assert(quad.mesh.bounds.min[0] == -1.0f);
    assert(quad.mesh.bounds.max[2] == 1.0f);

    const auto negative_indices = import_text(
        "v 0 0 0\n"
        "v 0 0 1\n"
        "v 1 0 0\n"
        "f -3 -2 -1\n");
    assert(negative_indices);
    assert(negative_indices.mesh.triangle_indices[2] == 2);

    assert(!import_text("v 0 0 0\nf 1 2 3\n"));
    assert(!import_text("v nan 0 0\nv 0 0 1\nv 1 0 0\nf 1 2 3\n"));
    assert(!import_text("v 0 0 0\nv 1 0 0\nv 2 0 0\nf 1 2 3\n"));
    assert(!import_text("v 0 0 0\nv 0 0 1\nv 1 0 0\nf 1 2 4\n"));
    return 0;
}
