"""Helpers for local macOS ad-hoc codesigned build artifacts."""

def macos_codesigned_artifact(name, src, out, executable = False, visibility = None):
    """Copies and ad-hoc signs a macOS artifact while preserving its runtime name."""
    chmod = "chmod u+w \"$@\""
    if executable:
        chmod = chmod + " && chmod u+x \"$@\""

    native.genrule(
        name = name,
        srcs = [src],
        outs = [out],
        cmd = "mkdir -p \"$(@D)\" && cp \"$(location %s)\" \"$@\" && %s && codesign --force --sign - \"$@\"" % (src, chmod),
        executable = executable,
        target_compatible_with = select({
            "//engine:macos": [],
            "//conditions:default": ["@platforms//:incompatible"],
        }),
        visibility = visibility,
    )
