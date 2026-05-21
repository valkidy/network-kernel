#include "ai/ai_context.h"

#include <cassert>
#include <cstdint>
#include <optional>

int main() {
    network_example::ai::AIContext context;
    context.set_feature("hasVisibleEnemy", true);
    context.set_feature("hp01", 0.75f);
    context.set_feature("nearestEnemyId", static_cast<std::uint32_t>(42));
    context.set_feature("mode", std::string("combat"));

    assert(context.has_feature("hasVisibleEnemy"));
    assert(context.get_bool("hasVisibleEnemy") == std::optional<bool>(true));
    assert(context.get_float("hp01").has_value());
    assert(*context.get_float("hp01") > 0.74f);
    assert(context.get_uint32("nearestEnemyId") ==
           std::optional<std::uint32_t>(42));
    assert(context.get_string("mode") == std::optional<std::string>("combat"));

    assert(!context.has_feature("missing"));
    assert(!context.get_bool("hp01").has_value());
    assert(!context.get_float("hasVisibleEnemy").has_value());
    assert(!context.get_uint32("mode").has_value());

    context.clear();
    assert(!context.has_feature("hasVisibleEnemy"));
    return 0;
}
