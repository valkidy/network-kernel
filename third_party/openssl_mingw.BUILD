# -------------------------------------------------------------------
# OpenSSL prebuilt for MinGW-w64 x86_64
#
# Source:
#   https://github.com/openssl/openssl/releases/tag/openssl-4.0.0
#
# Build environment:
#   macOS + Homebrew mingw-w64
#
# Example build command:
#
#   ./Configure mingw64 \
#       --cross-compile-prefix=x86_64-w64-mingw32- \
#       no-shared \
#       no-tests \
#       --prefix=/absolute/path/to/openssl_x86_64
#
#   make -j$(sysctl -n hw.ncpu)
#   make install_sw
#
# Output format:
#   Windows PE/COFF static libraries for MinGW-w64
#
# Notes:
#   - DO NOT use Homebrew macOS OpenSSL libraries for Windows targets.
#   - macOS Homebrew OpenSSL archives are Mach-O format and incompatible
#     with MinGW-w64 Windows linking.
#   - These prebuilt libraries are intended for:
#       macOS host -> Windows x86_64 target cross-compilation
# -------------------------------------------------------------------


package(default_visibility = ["//visibility:public"])

cc_library(
    name = "openssl_static",
    hdrs = glob([
        "include/openssl/**/*.h",
        "include/crypto/**/*.h",
    ]),
    includes = ["include"],
    srcs = [
        "lib/libssl.a",
        "lib/libcrypto.a",
    ],
    linkopts = [
        "-lcrypt32",
        "-lws2_32",
        "-lbcrypt",
    ],
)

cc_library(
    name = "openssl_import",
    hdrs = glob([
        "include/openssl/**/*.h",
        "include/crypto/**/*.h",
    ]),
    includes = ["include"],
    srcs = [
        "lib/libssl.dll.a",
        "lib/libcrypto.dll.a",
    ],
    data = [
        "lib/libssl-4-x64.dll",
        "lib/libcrypto-4-x64.dll",
    ],
    linkopts = [
        "-lcrypt32",
        "-lws2_32",
        "-lbcrypt",
    ],
)

alias(
    name = "openssl",
    actual = ":openssl_static",
)
