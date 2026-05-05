#include <Eigen/Core>
#include <entt/entt.hpp>
#include <flatbuffers/flatbuffers.h>
#include <fmt/format.h>
#include <cstdint>
#include <glm/glm.hpp>
#include <iostream>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <steam/steamnetworkingtypes.h>
#include <string>

int main() {
    entt::registry registry;
    const auto entity = registry.create();

    const Eigen::Vector2f eigen_vector(1.0f, 2.0f);
    const glm::vec2 glm_vector(3.0f, 4.0f);

    flatbuffers::FlatBufferBuilder builder;
    const auto flatbuffer_string = builder.CreateString("network-example");

    SteamNetworkingIdentity identity;
    identity.Clear();

    const nlohmann::json payload = {
        {"entity", static_cast<std::uint32_t>(entity)},
        {"flatbuffer_offset", flatbuffer_string.o},
        {"x", eigen_vector.x() + glm_vector.x},
    };

    const std::string message = fmt::format("third-party smoke: {}", payload.dump());
    spdlog::info("{}", message);
    std::cout << message;
    return 0;
}
