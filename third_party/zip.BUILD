licenses(["notice"])

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "zip",
    srcs = ["src/zip.c"],
    hdrs = [
        "src/miniz.h",
        "src/zip.h",
    ],
    copts = [
        "-D_DARWIN_C_SOURCE",
        "-DZIP_ENABLE_DEFLATE=0",
        "-DZIP_HAVE_SYMLINK=0",
    ],
    includes = ["src"],
)
