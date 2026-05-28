licenses(["notice"])

package(default_visibility = ["//visibility:public"])

GNS_COMMON_SRCS = [
    "src/common/crypto_textencode.cpp",
    "src/common/keypair.cpp",
    "src/common/steamid.cpp",
    "src/steamnetworkingsockets/steamnetworkingsockets_certs.cpp",
    "src/steamnetworkingsockets/steamnetworkingsockets_certstore.cpp",
    "src/steamnetworkingsockets/steamnetworkingsockets_shared.cpp",
    "src/tier0/dbg.cpp",
    "src/tier0/platformtime.cpp",
    "src/tier0/valve_tracelogging.cpp",
    "src/tier1/ipv6text.c",
    "src/tier1/netadr.cpp",
    "src/tier1/utlbuffer.cpp",
    "src/tier1/utlmemory.cpp",
    "src/vstdlib/strtools.cpp",
]

GNS_OPENSSL_SRCS = [
    "src/common/crypto.cpp",
    "src/common/crypto_25519_openssl.cpp",
    "src/common/crypto_digest_opensslevp.cpp",
    "src/common/crypto_openssl.cpp",
    "src/common/crypto_symmetric_opensslevp.cpp",
    "src/common/opensslwrapper.cpp",
]

GNS_BCRYPT_SRCS = [
    "src/common/crypto.cpp",
    "src/common/crypto_25519_donna.cpp",
    "src/common/crypto_bcrypt.cpp",
    "src/external/curve25519-donna/curve25519.c",
    "src/external/curve25519-donna/curve25519_VALVE_sse2.c",
    "src/external/ed25519-donna/ed25519_VALVE.c",
    "src/external/ed25519-donna/ed25519_VALVE_sse2.c",
]

GNS_CLIENTLIB_SRCS = [
    "src/steamnetworkingsockets/steamnetworkingsockets_stats.cpp",
    "src/steamnetworkingsockets/steamnetworkingsockets_thinker.cpp",
    "src/steamnetworkingsockets/clientlib/csteamnetworkingmessages.cpp",
    "src/steamnetworkingsockets/clientlib/csteamnetworkingsockets.cpp",
    "src/steamnetworkingsockets/clientlib/steamnetworkingsockets_connections.cpp",
    "src/steamnetworkingsockets/clientlib/steamnetworkingsockets_flat.cpp",
    "src/steamnetworkingsockets/clientlib/steamnetworkingsockets_lowlevel.cpp",
    "src/steamnetworkingsockets/clientlib/steamnetworkingsockets_p2p.cpp",
    "src/steamnetworkingsockets/clientlib/steamnetworkingsockets_snp.cpp",
    "src/steamnetworkingsockets/clientlib/steamnetworkingsockets_udp.cpp",
]

cc_library(
    name = "steam_headers",
    hdrs = glob([
        "include/steam/**/*.h",
    ]),
    includes = ["include"],
)

proto_library(
    name = "gns_common_proto",
    srcs = [
        "src/common/steamnetworkingsockets_messages.proto",
        "src/common/steamnetworkingsockets_messages_certs.proto",
    ],
    strip_import_prefix = "src/common",
)

proto_library(
    name = "gns_udp_proto",
    srcs = ["src/common/steamnetworkingsockets_messages_udp.proto"],
    strip_import_prefix = "src/common",
    deps = [":gns_common_proto"],
)

cc_proto_library(
    name = "gns_common_cc_proto",
    deps = [":gns_common_proto"],
)

cc_proto_library(
    name = "gns_udp_cc_proto",
    deps = [":gns_udp_proto"],
)

cc_library(
    name = "gns_static",
    srcs = GNS_COMMON_SRCS + GNS_CLIENTLIB_SRCS + select({
        "@//engine:mingw_w64": GNS_BCRYPT_SRCS,
        "//conditions:default": GNS_OPENSSL_SRCS,
    }),
    hdrs = glob([
        "include/steam/**/*.h",
        "src/**/*.h",
    ]),
    textual_hdrs = [
        "src/external/curve25519-donna/curve25519.c",
        "src/external/ed25519-donna/ed25519.c",
        "src/external/ed25519-donna/ed25519_VALVE.c",
    ],
    copts = [
        "-Wno-deprecated-declarations",
        "-Wno-format",
        "-Wno-microsoft-include",
        "-Wno-missing-field-initializers",
        "-Wno-nullability-completeness",
        "-Wno-reorder",
        "-Wno-sign-compare",
        "-Wno-unused-function",
        "-Wno-unused-local-typedef",
        "-Wno-unused-private-field",
        "-Wno-unused-variable",
    ],
    defines = [
        "STEAMNETWORKINGSOCKETS_STATIC_LINK",
    ],
    includes = [
        "include",
        "src",
        "src/common",
        "src/external/curve25519-donna",
        "src/external/ed25519-donna",
        "src/public",
        "src/steamnetworkingsockets",
        "src/steamnetworkingsockets/clientlib",
    ],
    local_defines = [
        "GOOGLE_PROTOBUF_NO_RTTI",
        "STEAMNETWORKINGSOCKETS_FOREXPORT",
        "STEAMNETWORKINGSOCKETS_OPENSOURCE",
        "VALVE_CRYPTO_ENABLE_25519",
    ] + select({
        "@//engine:mingw_w64": [
            "ED25519_HASH_BCRYPT",
            "VALVE_CRYPTO_25519_DONNA",
            "VALVE_CRYPTO_BCRYPT",
            "WINDOWS",
            "_WINDOWS",
        ],
        "//conditions:default": [
            "ENABLE_OPENSSLCONNECTION",
            "OSX",
            "VALVE_CRYPTO_25519_OPENSSLEVP",
            "VALVE_CRYPTO_OPENSSL",
        ],
    }),
    deps = [
        ":gns_common_cc_proto",
        ":gns_udp_cc_proto",
    ] + select({
        "@//engine:mingw_w64": [],
        "//conditions:default": ["@//third_party:openssl"],
    }),
)
