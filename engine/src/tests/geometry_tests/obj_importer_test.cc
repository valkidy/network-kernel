#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>

#include "geometry/public/obj_importer.h"

namespace {

void require_impl(bool condition, const char* expression, int line) {
    if (!condition) {
        std::fprintf(stderr, "require failed at line %d: %s\n", line, expression);
        std::abort();
    }
}

}  // namespace

#define require(condition) \
    require_impl(static_cast<bool>(condition), #condition, __LINE__)

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
    require(quad);
    require(quad.mesh.positions.size() == 4);
    require(quad.mesh.triangle_indices.size() == 6);
    require(quad.mesh.bounds.min[0] == -1.0f);
    require(quad.mesh.bounds.max[2] == 1.0f);

    const auto negative_indices = import_text(
        "v 0 0 0\n"
        "v 0 0 1\n"
        "v 1 0 0\n"
        "f -3 -2 -1\n");
    require(negative_indices);
    require(negative_indices.mesh.triangle_indices[2] == 2);

    require(!import_text("v 0 0 0\nf 1 2 3\n"));
    require(!import_text("v nan 0 0\nv 0 0 1\nv 1 0 0\nf 1 2 3\n"));
    require(!import_text("v 0 0 0\nv 1 0 0\nv 2 0 0\nf 1 2 3\n"));
    require(!import_text("v 0 0 0\nv 0 0 1\nv 1 0 0\nf 1 2 4\n"));
    return 0;
}
