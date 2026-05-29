package(default_visibility = ["//visibility:public"])

licenses(["notice"])

COPTS = select({
    # Don't pass any of the default flags as the VS Compiler does not support
    # these settings.
    "@platforms//os:windows": [],
    "//conditions:default": [
        "-Wno-dangling-else",
        "-Wno-format",
        "-Wno-implicit-function-declaration",
        "-Wno-incompatible-pointer-types",
        "-Wno-incompatible-pointer-types-discards-qualifiers",
        "-Wno-parentheses",
        "-DIOAPI_NO_64",
    ],
})

_ZLIB_HEADERS = [
    "crc32.h",
    "deflate.h",
    "gzguts.h",
    "inffast.h",
    "inffixed.h",
    "inflate.h",
    "inftrees.h",
    "trees.h",
    "zconf.h",
    "zlib.h",
    "zutil.h",
]

cc_library(
    name = "zlib",
    srcs = [
        "adler32.c",
        "compress.c",
        "crc32.c",
        "deflate.c",
        "gzclose.c",
        "gzlib.c",
        "gzread.c",
        "gzwrite.c",
        "infback.c",
        "inffast.c",
        "inflate.c",
        "inftrees.c",
        "trees.c",
        "uncompr.c",
        "zutil.c",
    ] + _ZLIB_HEADERS,
    hdrs = _ZLIB_HEADERS,
    copts = COPTS,
    includes = ["."],
)

cc_library(
    name = "zlib_minizip",
    srcs = [
        "contrib/minizip/ioapi.c",
        "contrib/minizip/unzip.c",
        "contrib/minizip/zip.c",
    ] + select({
        "@platforms//os:windows": [
            "contrib/minizip/iowin32.c",
            "contrib/minizip/iowin32.h",
        ],
        "//conditions:default": [],
    }),
    hdrs = [
        "contrib/minizip/crypt.h",
        "contrib/minizip/ioapi.h",
        "contrib/minizip/mztools.h",
        "contrib/minizip/unzip.h",
        "contrib/minizip/zip.h",
    ],
    copts = COPTS,
    deps = [":zlib"],
)
