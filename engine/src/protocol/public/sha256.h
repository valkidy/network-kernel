#ifndef PROTOCOL_PUBLIC_SHA256_H_
#define PROTOCOL_PUBLIC_SHA256_H_

#include <array>
#include <cstddef>
#include <cstdint>

namespace network_example {

std::array<std::uint8_t, 32> compute_sha256(
    const std::uint8_t* data,
    std::size_t size);

}  // namespace network_example

#endif  // PROTOCOL_PUBLIC_SHA256_H_
