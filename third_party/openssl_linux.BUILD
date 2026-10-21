# -------------------------------------------------------------------
# OpenSSL built from source for Linux x86_64
#
# Source:
#   https://github.com/openssl/openssl/releases/tag/openssl-4.0.3
#
# OpenSSL's own Configure + make run inside a genrule, out of tree, and
# only the static libraries and the generated public headers come back
# out. A hand-written cc_library over the sources is not an option: the
# public headers (opensslv.h, configuration.h, ssl.h, ...) and the
# assembly are produced by perl scripts at configure time.
#
# Host tools the genrule needs: perl, make, and a C compiler on PATH.
# The Linux builder image (tools/docker/builder.Dockerfile) has all three.
#
# -fPIC: libcrypto.a is linked into libnetwork_kernel.so as well as into
# the executable, and a shared library cannot take non-PIC objects.
# -------------------------------------------------------------------

package(default_visibility = ["//visibility:public"])

# Every public header that ships as a .h.in template. The genrule writes
# each one's configured .h under generated/, keeping the include/openssl/
# layout so `#include <openssl/ssl.h>` resolves through the includes below.
OPENSSL_GENERATED_HEADERS = [
    template[:-len(".in")]
    for template in glob(["include/openssl/*.h.in"])
]

genrule(
    name = "openssl_build",
    srcs = glob(["**"]),
    outs = [
        "lib/libssl.a",
        "lib/libcrypto.a",
    ] + ["generated/" + header for header in OPENSSL_GENERATED_HEADERS],
    cmd = """
set -euo pipefail
SRC_DIR="$$(cd "$$(dirname "$(location Configure)")" && pwd)"
OUT_DIR="$$(pwd)/$(RULEDIR)"
BUILD_DIR="$$(mktemp -d)"
trap 'rm -rf "$$BUILD_DIR"' EXIT
cd "$$BUILD_DIR"
if ! perl "$$SRC_DIR/Configure" no-shared no-tests -fPIC > build.log 2>&1 ||
   ! make -j"$$(nproc)" build_libs >> build.log 2>&1; then
    tail -n 80 build.log >&2
    exit 1
fi
mkdir -p "$$OUT_DIR/lib"
cp libssl.a libcrypto.a "$$OUT_DIR/lib/"
for header in {headers}; do
    mkdir -p "$$OUT_DIR/generated/$$(dirname "$$header")"
    cp "$$header" "$$OUT_DIR/generated/$$header"
done
""".replace("{headers}", " ".join(OPENSSL_GENERATED_HEADERS)),
)

cc_library(
    name = "openssl",
    srcs = [
        "lib/libssl.a",
        "lib/libcrypto.a",
    ],
    hdrs = glob(["include/openssl/*.h"]) +
           ["generated/" + header for header in OPENSSL_GENERATED_HEADERS],
    includes = [
        "generated/include",
        "include",
    ],
    linkopts = [
        "-ldl",
        "-lpthread",
    ],
    linkstatic = True,
)
