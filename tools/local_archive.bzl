def _sha256_of_file(ctx, p):
    result = ctx.execute([
        "/bin/sh",
        "-c",
        "shasum -a 256 \"$1\" | awk '{print $1}'",
        "sh",
        p,
    ])
    if result.return_code != 0:
        fail("failed to compute sha256 for {}: {}".format(p, result.stderr))
    return result.stdout.strip()

def _local_archive_impl(ctx):
    if len(ctx.attr.src) == 0:
        fail("src must not be empty")

    archive = ctx.path(ctx.attr.src[0])

    if ctx.attr.sha256:
        actual = _sha256_of_file(ctx, archive)
        if actual != ctx.attr.sha256:
            fail(
                "sha256 mismatch for {}: expected {}, got {}".format(
                    ctx.attr.src[0],
                    ctx.attr.sha256,
                    actual,
                )
            )

    ctx.extract(
        archive = archive,
        stripPrefix = ctx.attr.strip_prefix,
    )

    for p in ctx.attr.delete:
        ctx.delete(p)

    if ctx.attr.build_file:
        ctx.file("BUILD.bazel", ctx.read(ctx.attr.build_file))
    elif ctx.attr.build_file_content:
        ctx.file("BUILD.bazel", ctx.attr.build_file_content)
    else:
        fail("Exactly one of build_file and build_file_content must be specified.")

local_archive = repository_rule(
    implementation = _local_archive_impl,
    attrs = {
        "src": attr.label_list(),
        "sha256": attr.string(),
        "strip_prefix": attr.string(),
        "build_file": attr.label(allow_single_file = True),
        "build_file_content": attr.string(),
        "delete": attr.string_list(
            doc = "Paths (relative to repo root, post strip_prefix) to remove after extraction. " +
                  "Useful for stripping stray nested BUILD files that would create unwanted subpackages.",
        ),
    },
)
