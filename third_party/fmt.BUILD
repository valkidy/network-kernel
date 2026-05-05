licenses(["notice"])

package(default_visibility = ["//visibility:public"])

FMT_HEADERS = glob([
    "include/fmt/*.h",
])

cc_library(
    name = "fmt",
    srcs = [
        "src/format.cc",
        "src/os.cc",
    ],
    hdrs = FMT_HEADERS,
    includes = ["include"],
)

cc_library(
    name = "fmt-header-only",
    hdrs = FMT_HEADERS,
    defines = ["FMT_HEADER_ONLY=1"],
    includes = ["include"],
)
