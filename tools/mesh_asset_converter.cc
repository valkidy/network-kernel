#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

#include "geometry/public/mesh_bake.h"
#include "geometry/public/obj_importer.h"

namespace {

struct Arguments {
    std::string input;
    std::string config;
    std::string jolt_output;
    std::string recast_output;
};

bool set_argument(
    const std::string& argument,
    const std::string& prefix,
    std::string* value) {
    if (!argument.starts_with(prefix)) {
        return false;
    }
    if (!value->empty()) {
        return false;
    }
    *value = argument.substr(prefix.size());
    return !value->empty();
}

bool parse_arguments(int argc, char** argv, Arguments* arguments) {
    for (int i = 1; i < argc; ++i) {
        const std::string argument(argv[i]);
        if (set_argument(argument, "--input=", &arguments->input) ||
            set_argument(argument, "--config=", &arguments->config) ||
            set_argument(argument, "--jolt-output=", &arguments->jolt_output) ||
            set_argument(argument, "--recast-output=", &arguments->recast_output)) {
            continue;
        }
        return false;
    }
    return !arguments->input.empty() && !arguments->config.empty() &&
           !arguments->jolt_output.empty() && !arguments->recast_output.empty() &&
           arguments->jolt_output != arguments->recast_output;
}

bool write_temp_file(
    const std::filesystem::path& path,
    const std::vector<std::uint8_t>& bytes,
    std::string* error) {
    std::error_code filesystem_error;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), filesystem_error);
        if (filesystem_error) {
            *error = "cannot create output directory: " +
                path.parent_path().string();
            return false;
        }
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        *error = "cannot open output: " + path.string();
        return false;
    }
    output.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output) {
        *error = "failed writing output: " + path.string();
        return false;
    }
    return true;
}

bool write_outputs_atomically(
    const Arguments& arguments,
    const network_example::MeshBakeArtifacts& artifacts,
    std::string* error) {
    const std::filesystem::path jolt_output(arguments.jolt_output);
    const std::filesystem::path recast_output(arguments.recast_output);
    const std::filesystem::path jolt_temp = jolt_output.string() + ".tmp";
    const std::filesystem::path recast_temp = recast_output.string() + ".tmp";
    std::error_code ignored;
    std::filesystem::remove(jolt_temp, ignored);
    std::filesystem::remove(recast_temp, ignored);
    if (!write_temp_file(jolt_temp, artifacts.jolt, error) ||
        !write_temp_file(recast_temp, artifacts.recast, error)) {
        std::filesystem::remove(jolt_temp, ignored);
        std::filesystem::remove(recast_temp, ignored);
        return false;
    }

    std::filesystem::remove(jolt_output, ignored);
    std::filesystem::remove(recast_output, ignored);
    std::error_code rename_error;
    std::filesystem::rename(jolt_temp, jolt_output, rename_error);
    if (rename_error) {
        *error = "cannot finalize Jolt output: " + rename_error.message();
        std::filesystem::remove(jolt_temp, ignored);
        std::filesystem::remove(recast_temp, ignored);
        return false;
    }
    std::filesystem::rename(recast_temp, recast_output, rename_error);
    if (rename_error) {
        *error = "cannot finalize Recast output: " + rename_error.message();
        std::filesystem::remove(jolt_output, ignored);
        std::filesystem::remove(recast_temp, ignored);
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Arguments arguments;
    if (!parse_arguments(argc, argv, &arguments)) {
        std::cerr
            << "usage: mesh_asset_converter --input=<obj> --config=<yaml> "
               "--jolt-output=<file> --recast-output=<file>\n";
        return 2;
    }

    const network_example::ObjImportResult imported =
        network_example::import_obj_file(arguments.input);
    if (!imported) {
        std::cerr << imported.error << '\n';
        return 1;
    }
    const network_example::MeshBakeConfigResult config =
        network_example::load_mesh_bake_config_file(arguments.config);
    if (!config) {
        std::cerr << config.error << '\n';
        return 1;
    }

    std::string error;
    const std::uint64_t source_hash =
        network_example::fnv1a_file_hash(arguments.input, &error);
    if (!error.empty()) {
        std::cerr << error << '\n';
        return 1;
    }
    const std::uint64_t config_hash =
        network_example::fnv1a_file_hash(arguments.config, &error);
    if (!error.empty()) {
        std::cerr << error << '\n';
        return 1;
    }

    network_example::MeshBakeResult baked =
        network_example::bake_mesh_artifacts(
            imported.mesh, config.config, source_hash, config_hash);
    if (!baked) {
        std::cerr << baked.error << '\n';
        return 1;
    }
    if (!write_outputs_atomically(arguments, baked.artifacts, &error)) {
        std::cerr << error << '\n';
        return 1;
    }

    std::cout << "mesh_asset_converter: PASS\n"
              << "vertices=" << imported.mesh.positions.size()
              << " triangles=" << imported.mesh.triangle_indices.size() / 3
              << " jolt_bytes=" << baked.artifacts.jolt.size()
              << " recast_bytes=" << baked.artifacts.recast.size() << '\n';
    return 0;
}
