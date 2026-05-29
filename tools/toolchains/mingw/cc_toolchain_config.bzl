load(
    "@rules_cc//cc:cc_toolchain_config_lib.bzl",
    "artifact_name_pattern",
    "feature",
    "tool_path",
)
load("@rules_cc//cc/common:cc_common.bzl", "cc_common")

_MINGW_ROOT = "/opt/homebrew/Cellar/mingw-w64/14.0.0/toolchain-x86_64"
_MINGW_PREFIX = _MINGW_ROOT + "/bin/x86_64-w64-mingw32-"
_MINGW_GCC_VERSION = "15.2.0"
_MINGW_GCC_LIB = _MINGW_ROOT + "/lib/gcc/x86_64-w64-mingw32/" + _MINGW_GCC_VERSION

def _impl(ctx):
    tool_paths = [
        tool_path(name = "ar", path = _MINGW_PREFIX + "ar"),
        tool_path(name = "compat-ld", path = _MINGW_PREFIX + "ld"),
        tool_path(name = "cpp", path = _MINGW_PREFIX + "cpp"),
        tool_path(name = "dwp", path = "/bin/false"),
        tool_path(name = "gcc", path = _MINGW_PREFIX + "gcc"),
        tool_path(name = "gcov", path = _MINGW_PREFIX + "gcov"),
        tool_path(name = "ld", path = _MINGW_PREFIX + "ld"),
        tool_path(name = "nm", path = _MINGW_PREFIX + "nm"),
        tool_path(name = "objcopy", path = _MINGW_PREFIX + "objcopy"),
        tool_path(name = "objdump", path = _MINGW_PREFIX + "objdump"),
        tool_path(name = "strip", path = _MINGW_PREFIX + "strip"),
    ]

    features = [
        feature(name = "supports_pic", enabled = True),
    ]

    cxx_builtin_include_directories = [
        _MINGW_ROOT + "/x86_64-w64-mingw32/include/c++/" + _MINGW_GCC_VERSION,
        _MINGW_ROOT + "/x86_64-w64-mingw32/include/c++/" + _MINGW_GCC_VERSION + "/x86_64-w64-mingw32",
        _MINGW_ROOT + "/x86_64-w64-mingw32/include/c++/" + _MINGW_GCC_VERSION + "/backward",
        _MINGW_GCC_LIB + "/include",
        _MINGW_GCC_LIB + "/include-fixed",
        _MINGW_ROOT + "/x86_64-w64-mingw32/include",
    ]

    artifact_name_patterns = [
        artifact_name_pattern(
            category_name = "executable",
            prefix = "",
            extension = ".exe",
        ),
    ]

    return cc_common.create_cc_toolchain_config_info(
        ctx = ctx,
        features = features,
        artifact_name_patterns = artifact_name_patterns,
        cxx_builtin_include_directories = cxx_builtin_include_directories,
        toolchain_identifier = "local-mingw-w64",
        host_system_name = "local",
        target_system_name = "x86_64-w64-mingw32",
        target_cpu = "x86_64_windows",
        target_libc = "mingw",
        compiler = "mingw-gcc",
        abi_version = "unknown",
        abi_libc_version = "unknown",
        tool_paths = tool_paths,
        builtin_sysroot = _MINGW_ROOT,
    )

mingw_cc_toolchain_config = rule(
    implementation = _impl,
    attrs = {},
    provides = [CcToolchainConfigInfo],
)
