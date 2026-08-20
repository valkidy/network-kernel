// Locomotion capture driver, driven through the public Kernel/GameServer C ABI.
//
// This harness exercises the exact C ABI entry points the Unity plugin's
// managed NetworkHost/Kernel wrappers P/Invoke into, in the same order
// NetworkHost.Start / NetworkHost.Update use:
//
//   Kernel_Create
//   -> GameServer_CreateWithGameplayCatalogFromMemory(bundle, entry)
//   -> Kernel_SetGameplayCatalogSyncBundle(bundle, entry, namespace)
//   -> Kernel_StartListenServer(port)
//   per tick:
//     Kernel_Update(1/rate)
//     Kernel_PollEvents -> GameServer_HandleEvent
//     GameServer_Tick(1/rate)
//     Kernel_ServerCreateEntity        (once, the capture subject)
//     Kernel_ServerGetEntityState      (path script feedback)
//     Kernel_ServerSubmitEntityInput   (scripted move input)
//     Kernel_GetRenderStates           (root presentation state)
//     Kernel_GetSkeletonRenderStates   (local skeleton pose)
//
// The kernel symbols are resolved from a dlopen'd shared library so the same
// binary can drive either the freshly built kernel (native baseline) or the
// dylib shipped inside the Unity package (plugin baseline). The only difference
// between those two runs is (dylib, bundle) -- everything else is identical,
// which is the layer-1 "native raw <-> plugin raw" parity the test guideline
// asks for.
//
// The capture subject is created by this harness rather than left to the
// catalog's sentry AI, so the path is ours to script. The game server's agent
// runtime still adopts the entity and submits its own patrol input; the kernel
// resolves competing inputs for one entity by highest input_seq (see
// latest_input_by_net_id in engine/src/simulation/src/player_movement.cc), so
// the harness submits from kScriptedInputSeqBase and always wins.
//
// Usage (normally invoked by the //engine/src/tests/kernel_tests:locomotion_capture
// orchestrator, which fills in every path):
//
//   locomotion_capture_driver --dylib=PATH --bundle=PATH --manifest=PATH
//                             --out-prefix=PREFIX [--entry=NAME]
//                             [--samples=300] [--tick-rate=30] [--path="+X"]
//                             [--entity-template=21] [--spawn=x,y,z]
//                             [--port=7777] [--label=native]
//
// Writes <out-prefix>_root.csv and <out-prefix>_bones.csv.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <dlfcn.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "capture/path_script.h"
#include "capture/skeleton_manifest.h"
#include "capture/transform_capture.h"
#include "game_server/public/game_server_api.h"
#include "kernel/public/kernel_api.h"
#include "kernel/public/kernel_types.h"

namespace {

using network_example::capture::EntitySample;
using network_example::capture::EntityTransformWriter;
using network_example::capture::HierarchyTransformWriter;
using network_example::capture::LocalTransform;
using network_example::capture::PathScript;
using network_example::capture::SkeletonManifest;

// Beats the game server's sentry patrol input, which counts up from 0.
constexpr std::uint32_t kScriptedInputSeqBase = 1000000u;
constexpr std::uint32_t kMaxSkeletonStates = 64u;
constexpr std::uint32_t kMaxBoneTransforms = 64u * 128u;

struct Options {
    std::string dylib_path;
    std::string bundle_path;
    std::string manifest_path;
    std::string out_prefix;
    std::string entry_path = "legged_locomotion_gameplay_catalog.yaml";
    std::string content_namespace = "legged_locomotion";
    std::string path_text = "+X";
    std::string label = "capture";
    int samples = 300;
    int tick_rate = 30;
    std::uint32_t entity_template_id = 20u;
    glm::vec3 spawn_position{0.0f, 10.0f, 0.0f};
    std::uint16_t port = 7777u;
};

std::vector<std::uint8_t> read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::fprintf(stderr, "failed to open %s\n", path.c_str());
        return {};
    }
    return std::vector<std::uint8_t>(
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>());
}

bool parse_vec3(const std::string& text, glm::vec3* out_value) {
    float values[3] = {0.0f, 0.0f, 0.0f};
    std::size_t begin = 0;
    for (int index = 0; index < 3; ++index) {
        const std::size_t comma = text.find(',', begin);
        const std::string part = text.substr(
            begin,
            comma == std::string::npos ? std::string::npos : comma - begin);
        if (part.empty()) {
            return false;
        }
        char* end = nullptr;
        values[index] = std::strtof(part.c_str(), &end);
        if (end == part.c_str()) {
            return false;
        }
        if (index < 2) {
            if (comma == std::string::npos) {
                return false;
            }
            begin = comma + 1;
        }
    }
    *out_value = glm::vec3(values[0], values[1], values[2]);
    return true;
}

bool parse_options(int argc, char** argv, Options* out_options) {
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        const std::size_t equals = argument.find('=');
        if (argument.rfind("--", 0) != 0 || equals == std::string::npos) {
            std::fprintf(stderr, "unrecognized argument: %s\n", argument.c_str());
            return false;
        }
        const std::string key = argument.substr(2, equals - 2);
        const std::string value = argument.substr(equals + 1);
        if (key == "dylib") {
            out_options->dylib_path = value;
        } else if (key == "bundle") {
            out_options->bundle_path = value;
        } else if (key == "manifest") {
            out_options->manifest_path = value;
        } else if (key == "out-prefix") {
            out_options->out_prefix = value;
        } else if (key == "entry") {
            out_options->entry_path = value;
        } else if (key == "namespace") {
            out_options->content_namespace = value;
        } else if (key == "path") {
            out_options->path_text = value;
        } else if (key == "label") {
            out_options->label = value;
        } else if (key == "samples") {
            out_options->samples = std::atoi(value.c_str());
        } else if (key == "tick-rate") {
            out_options->tick_rate = std::atoi(value.c_str());
        } else if (key == "entity-template") {
            out_options->entity_template_id = static_cast<std::uint32_t>(
                std::strtoul(value.c_str(), nullptr, 10));
        } else if (key == "port") {
            out_options->port = static_cast<std::uint16_t>(
                std::strtoul(value.c_str(), nullptr, 10));
        } else if (key == "spawn") {
            if (!parse_vec3(value, &out_options->spawn_position)) {
                std::fprintf(stderr, "invalid --spawn=%s (expected x,y,z)\n",
                             value.c_str());
                return false;
            }
        } else {
            std::fprintf(stderr, "unknown option: --%s\n", key.c_str());
            return false;
        }
    }

    const struct {
        const char* name;
        const std::string* value;
    } required[] = {
        {"--dylib", &out_options->dylib_path},
        {"--bundle", &out_options->bundle_path},
        {"--manifest", &out_options->manifest_path},
        {"--out-prefix", &out_options->out_prefix},
    };
    for (const auto& entry : required) {
        if (entry.value->empty()) {
            std::fprintf(stderr, "missing required option %s\n", entry.name);
            return false;
        }
    }
    if (out_options->samples <= 0) {
        std::fprintf(stderr, "--samples must be positive\n");
        return false;
    }
    if (out_options->tick_rate <= 0) {
        std::fprintf(stderr, "--tick-rate must be positive\n");
        return false;
    }
    return true;
}

template <typename Signature>
Signature* load_symbol(void* library, const char* name) {
    dlerror();
    void* symbol = dlsym(library, name);
    const char* error = dlerror();
    if (error != nullptr || symbol == nullptr) {
        std::fprintf(stderr, "failed to resolve %s: %s\n", name,
                     error == nullptr ? "null symbol" : error);
        std::abort();
    }
    return reinterpret_cast<Signature*>(symbol);
}

// Function pointer table for the subset of the C ABI this harness drives.
struct KernelAbi {
    KernelHandle* (*create)(const KernelConfig*);
    void (*destroy)(KernelHandle*);
    bool (*set_sync_bundle)(
        KernelHandle*,
        const KernelGameplayCatalogSyncServerConfig*,
        KernelGameplayCatalogManifest*);
    bool (*start_listen_server)(KernelHandle*, std::uint16_t);
    void (*update)(KernelHandle*, float);
    std::uint32_t (*poll_events)(KernelHandle*, KernelEvent*, std::uint32_t);
    std::uint32_t (*get_render_states)(
        KernelHandle*, RenderEntityState*, std::uint32_t);
    std::uint32_t (*get_skeleton_render_states)(
        KernelHandle*,
        KernelSkeletonRenderState*,
        std::uint32_t,
        KernelBoneLocalTransform*,
        std::uint32_t,
        KernelSkeletonRenderStateResult*);
    bool (*server_create_entity)(
        KernelHandle*, const KernelServerEntityCreateInfo*, std::uint32_t*);
    bool (*server_get_entity_state)(
        KernelHandle*, std::uint32_t, KernelServerEntityState*);
    bool (*server_submit_entity_input)(
        KernelHandle*, std::uint32_t, const KernelPlayerInput*);
    GameServerHandle* (*gs_create_with_catalog)(
        KernelHandle*,
        const std::uint8_t*,
        std::uint32_t,
        const char*,
        KernelGameplayCatalogLoadResult*);
    void (*gs_destroy)(GameServerHandle*);
    void (*gs_handle_event)(GameServerHandle*, const KernelEvent*);
    void (*gs_tick)(GameServerHandle*, float);
};

KernelAbi bind_abi(void* library) {
    KernelAbi abi{};
    abi.create =
        load_symbol<KernelHandle*(const KernelConfig*)>(library, "Kernel_Create");
    abi.destroy = load_symbol<void(KernelHandle*)>(library, "Kernel_Destroy");
    abi.set_sync_bundle = load_symbol<bool(
        KernelHandle*,
        const KernelGameplayCatalogSyncServerConfig*,
        KernelGameplayCatalogManifest*)>(
        library, "Kernel_SetGameplayCatalogSyncBundle");
    abi.start_listen_server = load_symbol<bool(KernelHandle*, std::uint16_t)>(
        library, "Kernel_StartListenServer");
    abi.update =
        load_symbol<void(KernelHandle*, float)>(library, "Kernel_Update");
    abi.poll_events =
        load_symbol<std::uint32_t(KernelHandle*, KernelEvent*, std::uint32_t)>(
            library, "Kernel_PollEvents");
    abi.get_render_states = load_symbol<std::uint32_t(
        KernelHandle*, RenderEntityState*, std::uint32_t)>(
        library, "Kernel_GetRenderStates");
    abi.get_skeleton_render_states = load_symbol<std::uint32_t(
        KernelHandle*,
        KernelSkeletonRenderState*,
        std::uint32_t,
        KernelBoneLocalTransform*,
        std::uint32_t,
        KernelSkeletonRenderStateResult*)>(
        library, "Kernel_GetSkeletonRenderStates");
    abi.server_create_entity = load_symbol<bool(
        KernelHandle*, const KernelServerEntityCreateInfo*, std::uint32_t*)>(
        library, "Kernel_ServerCreateEntity");
    abi.server_get_entity_state = load_symbol<bool(
        KernelHandle*, std::uint32_t, KernelServerEntityState*)>(
        library, "Kernel_ServerGetEntityState");
    abi.server_submit_entity_input = load_symbol<bool(
        KernelHandle*, std::uint32_t, const KernelPlayerInput*)>(
        library, "Kernel_ServerSubmitEntityInput");
    abi.gs_create_with_catalog = load_symbol<GameServerHandle*(
        KernelHandle*,
        const std::uint8_t*,
        std::uint32_t,
        const char*,
        KernelGameplayCatalogLoadResult*)>(
        library, "GameServer_CreateWithGameplayCatalogFromMemory");
    abi.gs_destroy =
        load_symbol<void(GameServerHandle*)>(library, "GameServer_Destroy");
    abi.gs_handle_event =
        load_symbol<void(GameServerHandle*, const KernelEvent*)>(
            library, "GameServer_HandleEvent");
    abi.gs_tick =
        load_symbol<void(GameServerHandle*, float)>(library, "GameServer_Tick");
    return abi;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse_options(argc, argv, &options)) {
        return 2;
    }

    PathScript path;
    std::string path_error;
    if (!PathScript::parse(
            options.path_text,
            static_cast<float>(options.tick_rate),
            &path,
            &path_error)) {
        std::fprintf(stderr, "invalid --path=%s: %s\n",
                     options.path_text.c_str(), path_error.c_str());
        return 2;
    }

    SkeletonManifest manifest;
    std::string manifest_error;
    if (!network_example::capture::load_skeleton_manifest(
            options.manifest_path, &manifest, &manifest_error)) {
        std::fprintf(stderr, "%s\n", manifest_error.c_str());
        return 1;
    }

    const std::vector<std::uint8_t> bundle = read_file(options.bundle_path);
    if (bundle.empty()) {
        return 1;
    }

    void* library = dlopen(options.dylib_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (library == nullptr) {
        std::fprintf(stderr, "dlopen(%s) failed: %s\n",
                     options.dylib_path.c_str(), dlerror());
        return 1;
    }
    const KernelAbi abi = bind_abi(library);

    const float fixed_delta = 1.0f / static_cast<float>(options.tick_rate);

    // --- Mirror NetworkHost.Start(port, config, catalog). ---
    KernelConfig config{};
    config.mode = KernelMode_ListenServer;
    config.tick.server_tick_rate =
        static_cast<std::uint32_t>(options.tick_rate);
    // KernelConfig.CreateDefault(ListenServer) halves the tick rate.
    config.tick.snapshot_rate =
        static_cast<std::uint32_t>(options.tick_rate) / 2u;
    config.max_render_states = 256;
    config.max_events = 256;

    KernelHandle* kernel = abi.create(&config);
    if (kernel == nullptr) {
        std::fprintf(stderr, "Kernel_Create failed\n");
        return 1;
    }

    KernelGameplayCatalogLoadResult load_result{};
    load_result.struct_size = sizeof(load_result);
    GameServerHandle* game_server = abi.gs_create_with_catalog(
        kernel, bundle.data(), static_cast<std::uint32_t>(bundle.size()),
        options.entry_path.c_str(), &load_result);
    if (game_server == nullptr) {
        std::fprintf(stderr,
                     "GameServer_CreateWithGameplayCatalogFromMemory failed "
                     "(entry=%s)\n",
                     options.entry_path.c_str());
        return 1;
    }

    KernelGameplayCatalogSyncServerConfig sync_config{};
    sync_config.struct_size = sizeof(sync_config);
    sync_config.bundle_bytes = bundle.data();
    sync_config.bundle_size = static_cast<std::uint32_t>(bundle.size());
    sync_config.entry_path = options.entry_path.c_str();
    sync_config.content_namespace = options.content_namespace.c_str();
    KernelGameplayCatalogManifest sync_manifest{};
    sync_manifest.struct_size = sizeof(sync_manifest);
    if (!abi.set_sync_bundle(kernel, &sync_config, &sync_manifest)) {
        std::fprintf(stderr, "Kernel_SetGameplayCatalogSyncBundle failed\n");
        return 1;
    }

    if (!abi.start_listen_server(kernel, options.port)) {
        std::fprintf(stderr, "Kernel_StartListenServer failed\n");
        return 1;
    }

    std::fprintf(stderr,
                 "[%s] catalog loaded: version=%u hash=0x%016llx entry=%s\n"
                 "[%s] path: %s\n",
                 options.label.c_str(),
                 load_result.catalog_version,
                 static_cast<unsigned long long>(load_result.catalog_hash),
                 options.entry_path.c_str(),
                 options.label.c_str(),
                 path.description().c_str());

    // --- Output writers. ---
    EntityTransformWriter root_writer(options.out_prefix + "_root.csv");
    HierarchyTransformWriter bone_writer(
        options.out_prefix + "_bones.csv", manifest.nodes);
    if (!root_writer.ok() || !bone_writer.ok()) {
        std::fprintf(stderr, "%s%s\n", root_writer.error().c_str(),
                     bone_writer.error().c_str());
        return 1;
    }

    std::vector<KernelEvent> events(config.max_events);
    std::vector<RenderEntityState> render_states(config.max_render_states);
    std::vector<KernelSkeletonRenderState> skel_states(kMaxSkeletonStates);
    std::vector<KernelBoneLocalTransform> bone_transforms(kMaxBoneTransforms);
    std::vector<LocalTransform> pose(manifest.nodes.size());

    std::uint32_t subject_net_id = 0;
    std::uint32_t input_seq = kScriptedInputSeqBase;
    glm::vec3 subject_position = options.spawn_position;

    int recorded = 0;
    int tick_index = 0;
    // Cap the warm-up window so a mis-spawn can never spin forever.
    const int max_ticks = options.samples + 20 * options.tick_rate;
    for (; tick_index < max_ticks && recorded < options.samples; ++tick_index) {
        // --- Mirror NetworkHost.Update(dt, events). ---
        abi.update(kernel, fixed_delta);
        const std::uint32_t event_count = abi.poll_events(
            kernel, events.data(), static_cast<std::uint32_t>(events.size()));
        for (std::uint32_t index = 0; index < event_count; ++index) {
            abi.gs_handle_event(game_server, &events[index]);
        }
        abi.gs_tick(game_server, fixed_delta);

        // --- Create and drive the capture subject. ---
        if (subject_net_id == 0) {
            KernelServerEntityCreateInfo create_info{};
            create_info.struct_size = sizeof(create_info);
            create_info.owner_peer = 0;
            create_info.position = KernelVec3{
                options.spawn_position.x,
                options.spawn_position.y,
                options.spawn_position.z};
            create_info.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
            create_info.entity_template_id = options.entity_template_id;
            if (!abi.server_create_entity(
                    kernel, &create_info, &subject_net_id) ||
                subject_net_id == 0) {
                std::fprintf(stderr,
                             "[%s] Kernel_ServerCreateEntity failed for entity "
                             "template %u\n",
                             options.label.c_str(),
                             options.entity_template_id);
                return 1;
            }
            std::fprintf(stderr,
                         "[%s] capture subject net_id=%u spawned at "
                         "(%.3f, %.3f, %.3f)\n",
                         options.label.c_str(), subject_net_id,
                         options.spawn_position.x, options.spawn_position.y,
                         options.spawn_position.z);
        }

        KernelServerEntityState subject_state{};
        subject_state.struct_size = sizeof(subject_state);
        if (abi.server_get_entity_state(
                kernel, subject_net_id, &subject_state) &&
            subject_state.valid != 0u) {
            subject_position = glm::vec3(
                subject_state.position.x,
                subject_state.position.y,
                subject_state.position.z);
        }

        const glm::vec2 move = path.move_input(subject_position);
        KernelPlayerInput input{};
        input.input_seq = input_seq++;
        input.move = KernelVec2{move.x, move.y};
        abi.server_submit_entity_input(kernel, subject_net_id, &input);

        // --- Sample: root presentation + local skeleton pose. ---
        const std::uint32_t state_count = abi.get_render_states(
            kernel, render_states.data(),
            static_cast<std::uint32_t>(render_states.size()));
        const RenderEntityState* subject = nullptr;
        for (std::uint32_t index = 0; index < state_count; ++index) {
            if (render_states[index].net_id == subject_net_id) {
                subject = &render_states[index];
                break;
            }
        }
        if (subject == nullptr) {
            continue;  // Not visible to presentation yet.
        }

        KernelSkeletonRenderStateResult skel_result{};
        skel_result.struct_size = sizeof(skel_result);
        abi.get_skeleton_render_states(
            kernel, skel_states.data(),
            static_cast<std::uint32_t>(skel_states.size()),
            bone_transforms.data(),
            static_cast<std::uint32_t>(bone_transforms.size()), &skel_result);
        if (skel_result.status != KERNEL_SKELETON_RENDER_STATUS_SUCCESS) {
            continue;  // Pose not finalized yet.
        }

        // Join skeleton state to the subject by entity_net_id.
        const KernelSkeletonRenderState* skeleton = nullptr;
        for (std::uint32_t index = 0; index < skel_result.written_state_count;
             ++index) {
            if (skel_states[index].entity_net_id == subject_net_id) {
                skeleton = &skel_states[index];
                break;
            }
        }
        if (skeleton == nullptr || skeleton->bone_count != manifest.bone_count) {
            continue;  // Wait for the full pose.
        }

        for (std::uint32_t index = 0; index < skeleton->bone_count; ++index) {
            const KernelBoneLocalTransform& bone =
                bone_transforms[skeleton->first_bone_transform + index];
            pose[index].position = glm::vec3(
                bone.local_position.x,
                bone.local_position.y,
                bone.local_position.z);
            pose[index].rotation = glm::quat(
                bone.local_rotation.w,
                bone.local_rotation.x,
                bone.local_rotation.y,
                bone.local_rotation.z);
            pose[index].scale = glm::vec3(
                bone.local_scale.x, bone.local_scale.y, bone.local_scale.z);
        }

        const std::uint32_t sample = static_cast<std::uint32_t>(recorded);
        const double time_seconds =
            static_cast<double>(sample) * static_cast<double>(fixed_delta);
        const glm::vec3 root_position(
            subject->position.x, subject->position.y, subject->position.z);
        const glm::quat root_rotation(
            subject->rotation.w, subject->rotation.x, subject->rotation.y,
            subject->rotation.z);

        EntitySample entity_sample;
        entity_sample.sample = sample;
        entity_sample.tick = skeleton->pose_tick;
        entity_sample.time_seconds = time_seconds;
        entity_sample.net_id = subject->net_id;
        entity_sample.template_id = subject->template_id;
        entity_sample.position = root_position;
        entity_sample.rotation = root_rotation;
        entity_sample.velocity = glm::vec3(
            subject->velocity.x, subject->velocity.y, subject->velocity.z);
        root_writer.write(entity_sample);

        bone_writer.write_sample(
            sample,
            skeleton->pose_tick,
            time_seconds,
            skeleton->entity_net_id,
            pose,
            root_position,
            root_rotation);
        ++recorded;
    }

    abi.gs_destroy(game_server);
    abi.destroy(kernel);
    dlclose(library);

    std::fprintf(
        stderr,
        "[%s] recorded %d/%d samples over %d ticks (bone_count=%u "
        "hash=0x%016llx) -> %s_root.csv / %s_bones.csv\n",
        options.label.c_str(), recorded, options.samples, tick_index,
        manifest.bone_count,
        static_cast<unsigned long long>(manifest.content_hash),
        options.out_prefix.c_str(), options.out_prefix.c_str());
    if (recorded != options.samples) {
        std::fprintf(stderr, "[%s] ERROR: capture ended early\n",
                     options.label.c_str());
        return 1;
    }
    if (!path.finished() && path.steps().size() > 2) {
        std::fprintf(stderr,
                     "[%s] WARNING: the path script did not finish within %d "
                     "samples; raise --sampling to walk it out\n",
                     options.label.c_str(), options.samples);
    }
    return 0;
}
