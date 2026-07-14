licenses(["notice"])

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "recast",
    srcs = glob(["Recast/Source/*.cpp"]),
    hdrs = glob(["Recast/Include/*.h"]),
    includes = ["Recast/Include"],
)

cc_library(
    name = "detour",
    srcs = glob(["Detour/Source/*.cpp"]),
    hdrs = glob(["Detour/Include/*.h"]),
    includes = ["Detour/Include"],
)

filegroup(
    name = "dungeon_obj",
    srcs = ["RecastDemo/Bin/Meshes/dungeon.obj"],
)
