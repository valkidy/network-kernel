#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "protocol/public/sha256.h"

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

std::string hex(const std::array<std::uint8_t, 32>& digest) {
    constexpr char kDigits[] = "0123456789abcdef";
    std::string result;
    result.reserve(digest.size() * 2);
    for (std::uint8_t byte : digest) {
        result.push_back(kDigits[byte >> 4]);
        result.push_back(kDigits[byte & 0x0f]);
    }
    return result;
}

}  // namespace

int main() {
    const std::array<std::uint8_t, 32> empty =
        network_example::compute_sha256(nullptr, 0);
    require(
        hex(empty) ==
        "e3b0c44298fc1c149afbf4c8996fb924"
        "27ae41e4649b934ca495991b7852b855");

    constexpr std::uint8_t kAbc[] = {'a', 'b', 'c'};
    const std::array<std::uint8_t, 32> abc =
        network_example::compute_sha256(kAbc, sizeof(kAbc));
    require(
        hex(abc) ==
        "ba7816bf8f01cfea414140de5dae2223"
        "b00361a396177a9cb410ff61f20015ad");
    return 0;
}
