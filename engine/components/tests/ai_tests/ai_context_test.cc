#include "ai_context.h"

#include <cassert>
#include <cstdint>
#include <optional>

int main() {
    network_example::ai::AIContext context;
    context.set_feature("hasVisibleHostile", true);
    context.set_feature("hp01", 0.75f);
    context.set_feature("nearestHostileId", static_cast<std::uint32_t>(42));
    context.set_feature("mode", std::string("combat"));

    assert(context.has_feature("hasVisibleHostile"));
    assert(context.get_bool("hasVisibleHostile") == std::optional<bool>(true));
    assert(context.get_float("hp01").has_value());
    assert(*context.get_float("hp01") > 0.74f);
    assert(context.get_uint32("nearestHostileId") ==
           std::optional<std::uint32_t>(42));
    assert(context.get_string("mode") == std::optional<std::string>("combat"));

    assert(!context.has_feature("missing"));
    assert(!context.get_bool("hp01").has_value());
    assert(!context.get_float("hasVisibleHostile").has_value());
    assert(!context.get_uint32("mode").has_value());

    context.clear();
    assert(!context.has_feature("hasVisibleHostile"));
    return 0;
}
