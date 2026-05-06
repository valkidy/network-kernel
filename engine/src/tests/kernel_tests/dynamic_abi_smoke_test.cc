#include <array>
#include <cassert>
#include <cstdlib>
#include <filesystem>
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

}  // namespace

int main() {
    const std::filesystem::path library_path = find_shared_library();
    void* library = dlopen(library_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    assert(library != nullptr);

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

    KernelConfig config{};
    config.mode = KernelMode_ListenServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 30;

    KernelHandle* kernel = kernel_create(&config);
    assert(kernel != nullptr);
    assert(kernel_start_listen_server(kernel, 7777));

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
