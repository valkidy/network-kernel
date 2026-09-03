"""Rules for cooking game-server OBJ mesh assets."""

def _validate_stem(stem, source):
    if not stem:
        fail("OBJ asset has an empty stem: %s" % source)
    if stem[0] not in "abcdefghijklmnopqrstuvwxyz":
        fail("OBJ asset stem must start with a lowercase letter: %s" % source)
    for index in range(len(stem)):
        character = stem[index]
        if character not in "abcdefghijklmnopqrstuvwxyz0123456789_":
            fail("OBJ asset stem must be lower_snake_case: %s" % source)

def mesh_asset_bakes(
        name,
        srcs,
        config,
        converter = "//tools:mesh_asset_converter"):
    """Expands OBJ sources into one hermetic bake genrule per asset."""
    outputs = []
    seen_stems = {}
    for source in srcs:
        filename = source.split("/")[-1]
        if not filename.endswith(".obj"):
            fail("mesh bake source must use the .obj extension: %s" % source)
        stem = filename[:-4]
        _validate_stem(stem, source)
        if stem in seen_stems:
            fail("duplicate OBJ asset stem %s: %s and %s" % (
                stem,
                seen_stems[stem],
                source,
            ))
        seen_stems[stem] = source

        jolt_output = "generated/jolt/%s.joltmesh" % stem
        recast_output = "generated/recast/%s.navmesh" % stem
        native.genrule(
            name = "bake_%s" % stem,
            srcs = [source, config],
            outs = [jolt_output, recast_output],
            tools = [converter],
            cmd = " ".join([
                "\"$(location %s)\"" % converter,
                "--input=\"$(location %s)\"" % source,
                "--config=\"$(location %s)\"" % config,
                "--jolt-output=\"$(@D)/%s\"" % jolt_output,
                "--recast-output=\"$(@D)/%s\"" % recast_output,
            ]),
            message = "Cooking mesh asset %s" % stem,
        )
        outputs.extend([jolt_output, recast_output])

    native.filegroup(
        name = name,
        srcs = outputs,
    )
