#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "ozz/animation/runtime/skeleton.h"
#include "ozz/base/io/archive.h"
#include "ozz/base/io/stream.h"

namespace {

std::string argument_value(int argc, char** argv, std::string_view prefix) {
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument.starts_with(prefix)) {
            return std::string(argument.substr(prefix.size()));
        }
    }
    return {};
}

std::uint64_t fnv1a64(const std::vector<std::uint8_t>& bytes) {
    std::uint64_t hash = 14695981039346656037ull;
    for (const std::uint8_t byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string json_string(std::string_view value) {
    std::string escaped = "\"";
    for (const unsigned char character : value) {
        switch (character) {
        case '\\': escaped += "\\\\"; break;
        case '"': escaped += "\\\""; break;
        case '\b': escaped += "\\b"; break;
        case '\f': escaped += "\\f"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default:
            if (character < 0x20u) {
                escaped += "\\u00";
                constexpr char kHex[] = "0123456789abcdef";
                escaped.push_back(kHex[character >> 4u]);
                escaped.push_back(kHex[character & 0x0fu]);
            } else {
                escaped.push_back(static_cast<char>(character));
            }
        }
    }
    escaped.push_back('"');
    return escaped;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string skeleton_path =
        argument_value(argc, argv, "--skeleton=");
    const std::string output_path = argument_value(argc, argv, "--output=");
    const std::string asset_id_text =
        argument_value(argc, argv, "--asset-id=");
    const std::string asset_name = argument_value(argc, argv, "--name=");
    const std::string runtime_file =
        argument_value(argc, argv, "--runtime-file=");
    if (skeleton_path.empty() || output_path.empty() || asset_id_text.empty() ||
        asset_name.empty() || runtime_file.empty()) {
        std::cerr << "usage: ozz_skeleton_manifest --skeleton=<path> "
                     "--output=<path> --asset-id=<id> --name=<name> "
                     "--runtime-file=<relative-path>\n";
        return 2;
    }

    std::ifstream bytes_input(skeleton_path, std::ios::binary);
    const std::vector<std::uint8_t> bytes{
        std::istreambuf_iterator<char>(bytes_input),
        std::istreambuf_iterator<char>()};
    if (bytes_input.bad() || bytes.empty()) {
        std::cerr << "failed to read skeleton bytes: " << skeleton_path << '\n';
        return 1;
    }

    ozz::io::File skeleton_input(skeleton_path.c_str(), "rb");
    if (!skeleton_input.opened()) {
        std::cerr << "failed to open skeleton: " << skeleton_path << '\n';
        return 1;
    }
    ozz::io::IArchive archive(&skeleton_input);
    if (!archive.TestTag<ozz::animation::Skeleton>()) {
        std::cerr << "input is not an ozz skeleton: " << skeleton_path << '\n';
        return 1;
    }
    ozz::animation::Skeleton skeleton;
    archive >> skeleton;

    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    if (!output) {
        std::cerr << "failed to create manifest: " << output_path << '\n';
        return 1;
    }
    const std::uint64_t content_hash = fnv1a64(bytes);
    output << "{\n"
           << "  \"manifest_version\": 1,\n"
           << "  \"asset_id\": " << std::stoul(asset_id_text) << ",\n"
           << "  \"name\": " << json_string(asset_name) << ",\n"
           << "  \"content_hash\": \"0x" << std::hex << std::setw(16)
           << std::setfill('0') << content_hash << std::dec << "\",\n"
           << "  \"runtime_skeleton\": " << json_string(runtime_file) << ",\n"
           << "  \"bone_count\": " << skeleton.num_joints() << ",\n"
           << "  \"bones\": [\n";
    const auto names = skeleton.joint_names();
    const auto parents = skeleton.joint_parents();
    for (int index = 0; index < skeleton.num_joints(); ++index) {
        output << "    {\"index\": " << index
               << ", \"name\": " << json_string(names[index])
               << ", \"parent_index\": " << parents[index] << "}"
               << (index + 1 == skeleton.num_joints() ? "\n" : ",\n");
    }
    output << "  ]\n}\n";
    if (!output.good()) {
        std::cerr << "failed to write manifest: " << output_path << '\n';
        return 1;
    }
    std::cout << "skeleton manifest generated: joints="
              << skeleton.num_joints() << " hash=0x" << std::hex
              << content_hash << std::dec << '\n';
    return 0;
}
