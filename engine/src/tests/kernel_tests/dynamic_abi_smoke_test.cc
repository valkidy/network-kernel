#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include <dlfcn.h>

#include "kernel/public/kernel_api.h"

namespace {

template <typename Signature>
Signature* load_symbol(void* library, const char* name) {
    dlerror();
    void* symbol = dlsym(library, name);
    const char* error = dlerror();
    assert(error == nullptr);
    assert(symbol != nullptr);
    return reinterpret_cast<Signature*>(symbol);
}

std::filesystem::path runfiles_root() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    const char* test_workspace = std::getenv("TEST_WORKSPACE");
    assert(test_srcdir != nullptr);
    assert(test_workspace != nullptr);
    return std::filesystem::path(test_srcdir) / test_workspace;
}

std::filesystem::path find_shared_library() {
    const std::filesystem::path root = runfiles_root();
    const std::vector<std::filesystem::path> candidates = {
        root / "engine/src/kernel/libnetwork_kernel.dylib",
        root / "engine/src/kernel/libnetwork_kernel.so",
    };

    for (const std::filesystem::path& candidate : candidates) {
        if (std::filesystem::exists(candidate)) {
            return candidate;
        }
    }

    assert(false);
    return {};
}

std::string shell_quote(const std::string& value) {
    std::string quoted = "'";
    for (const char character : value) {
        if (character == '\'') {
            quoted += "'\\''";
        } else {
            quoted += character;
        }
    }
    quoted += "'";
    return quoted;
}

void verify_exported_symbols(const std::filesystem::path& library_path) {
#if defined(__APPLE__)
    const std::string command = "nm -gU " + shell_quote(library_path.string());
    FILE* pipe = popen(command.c_str(), "r");
    assert(pipe != nullptr);

    std::array<char, 1024> line{};
    std::vector<std::string> unexpected_symbols;
    while (fgets(line.data(), static_cast<int>(line.size()), pipe) != nullptr) {
        std::string text = line.data();
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
            text.pop_back();
        }
        if (text.empty()) {
            continue;
        }
        const std::size_t symbol_begin = text.find_last_of(' ');
        const std::string symbol =
            symbol_begin == std::string::npos ? text : text.substr(symbol_begin + 1);
        if (symbol.rfind("_Kernel_", 0) != 0) {
            unexpected_symbols.push_back(symbol);
        }
    }

    const int status = pclose(pipe);
    assert(status == 0);
    assert(unexpected_symbols.empty());
#else
    (void)library_path;
#endif
}

}  // namespace

int main() {
    const std::filesystem::path library_path = find_shared_library();
    verify_exported_symbols(library_path);

    void* library = dlopen(library_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    assert(library != nullptr);

    auto* kernel_get_abi_info =
        load_symbol<bool(KernelAbiInfo*, std::uint32_t)>(library, "Kernel_GetAbiInfo");
    auto* kernel_create =
        load_symbol<KernelHandle*(const KernelConfig*)>(library, "Kernel_Create");
    auto* kernel_destroy =
        load_symbol<void(KernelHandle*)>(library, "Kernel_Destroy");
    [[maybe_unused]] auto* kernel_start_client =
        load_symbol<bool(KernelHandle*, const char*)>(library, "Kernel_StartClient");
    [[maybe_unused]] auto* kernel_start_dedicated_server =
        load_symbol<bool(KernelHandle*, std::uint16_t)>(
            library,
            "Kernel_StartDedicatedServer");
    auto* kernel_start_listen_server =
        load_symbol<bool(KernelHandle*, std::uint16_t)>(
            library,
            "Kernel_StartListenServer");
    auto* kernel_update =
        load_symbol<void(KernelHandle*, float)>(library, "Kernel_Update");
    auto* kernel_submit_input =
        load_symbol<void(KernelHandle*, std::uint32_t, const PlayerInput*)>(
            library,
            "Kernel_SubmitInput");
    auto* kernel_get_render_states =
        load_symbol<std::uint32_t(KernelHandle*, RenderEntityState*, std::uint32_t)>(
            library,
            "Kernel_GetRenderStates");
    auto* kernel_poll_events =
        load_symbol<std::uint32_t(KernelHandle*, KernelEvent*, std::uint32_t)>(
            library,
            "Kernel_PollEvents");
    auto* kernel_get_local_player_info =
        load_symbol<bool(KernelHandle*, KernelLocalPlayerInfo*)>(
            library,
            "Kernel_GetLocalPlayerInfo");

    KernelAbiInfo abi_info{};
    assert(kernel_get_abi_info(&abi_info, sizeof(abi_info)));
    assert(abi_info.abi_version == KERNEL_ABI_VERSION);
    assert(abi_info.kernel_config_size == sizeof(KernelConfig));
    assert(abi_info.player_input_size == sizeof(PlayerInput));
    assert(abi_info.render_entity_state_size == sizeof(RenderEntityState));
    assert(abi_info.kernel_event_size == sizeof(KernelEvent));
    assert(abi_info.local_player_info_size == sizeof(KernelLocalPlayerInfo));
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_LISTEN_SERVER_MODE) != 0);
    assert((abi_info.capability_flags & KERNEL_CAPABILITY_LOCAL_PLAYER_INFO) != 0);
    assert(!kernel_get_abi_info(nullptr, sizeof(abi_info)));
    assert(!kernel_get_abi_info(&abi_info, sizeof(abi_info) - 1));
    KernelLocalPlayerInfo local_info{};
    assert(!kernel_get_local_player_info(nullptr, &local_info));

    assert(kernel_create(nullptr) == nullptr);

    KernelConfig config{};
    config.mode = KernelMode_ListenServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 30;

    KernelHandle* kernel = kernel_create(&config);
    assert(kernel != nullptr);
    assert(kernel_start_listen_server(kernel, 7777));
    assert(kernel_get_local_player_info(kernel, &local_info));
    assert(local_info.peer_id == 1);
    assert(local_info.player_net_id != 0);
    assert(local_info.has_welcome);
    assert(local_info.connected);

    PlayerInput input{};
    input.input_seq = 1;
    input.client_tick = 1;
    input.move = KernelVec2{1.0f, 0.0f};
    input.aim_dir = KernelVec3{1.0f, 0.0f, 0.0f};
    input.buttons = InputButton_Fire;
    kernel_submit_input(kernel, 1, &input);
    kernel_update(kernel, 1.0f / 30.0f);

    std::array<RenderEntityState, 16> states{};
    const std::uint32_t state_count = kernel_get_render_states(
        kernel,
        states.data(),
        static_cast<std::uint32_t>(states.size()));
    assert(state_count >= 2);

    std::array<KernelEvent, 16> events{};
    const std::uint32_t event_count = kernel_poll_events(
        kernel,
        events.data(),
        static_cast<std::uint32_t>(events.size()));
    assert(event_count >= 1);

    kernel_destroy(kernel);
    assert(dlclose(library) == 0);
    return 0;
}
