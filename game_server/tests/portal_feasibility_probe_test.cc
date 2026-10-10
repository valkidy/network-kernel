// What a portal's teleport costs on the wire, measured before any portal code
// exists (docs/PORTAL_PLAN.md, P0). The portal's teleport will be the server
// moving a player in one tick; Kernel_ServerSetEntityTransform already does
// exactly that, so it stands in for it here.
//
// One dedicated server and two real clients over localhost, all three on the
// shipped catalog and ground, with nothing but the public API -- what Unity
// runs. The traveller keeps walking through the teleport, as a player walking
// into a portal would.
//
// R1. The traveller's own drawing. Does the predicted body jump once to where
//     the server put it, or slide there, and does prediction survive it?
// R2. An observer within relevance of both ends. Does it draw the traveller
//     jumping, or sliding through every point in between?
// R3. Relevance at the far end. After a jump past the relevance radius, how
//     many frames until the traveller's client is drawing what stands there?
//
// localhost delivers within a frame, so these are the mechanism's own
// numbers; a real round trip adds to every wait below.
//
// Measured 2026-10-10 (30 Hz ticks and snapshots, 30 m jump for R1/R2):
// R1. One 33.6 m snap, drawn where the server has it 2 frames after the
//     teleport; never drawn in between; prediction stays alive.
// R2. The observer draws ONE frame in between -- (-8.65, 0, 3.98), on the line
//     from the old spot to the new -- then snaps the rest: interpolation blends
//     across the last snapshot before the jump and the first after it. Caught
//     up after 5 frames, then the usual ~0.65 m interpolation lag behind a
//     walking body. At a lower snapshot rate, or a render rate above 30 fps,
//     that blend is drawn for more frames.
// R3. 94 m, past the 40 m relevance radius: the observer and both tents
//     standing there are drawn 2 frames after the teleport.
//
// P3 (K5) adds a fourth case, through a real portal_entrance rather than
// Kernel_ServerSetEntityTransform:
// R2'. The same observer, now told by an ActorTeleportBatch: no frame in
//     between, and both clients get KernelEventType_ActorTeleported naming
//     the portal's template.
//
// Every case prints what it measured. The requires pin only the controls, so
// "nothing happened" cannot pass for an answer.

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "game_server/src/game_server.h"
#include "game_server/src/gameplay_config.h"
#include "kernel/public/kernel_api.h"

namespace {

void require_impl(bool condition, int line, const char* text) {
    if (condition) {
        return;
    }
    std::fprintf(stderr, "require failed at line %d: %s\n", line, text);
    std::abort();
}

#define require(expr) require_impl(static_cast<bool>(expr), __LINE__, #expr)

using network_example::game_server::GameServerGameplayConfig;

constexpr float kTickSeconds = 1.0f / 30.0f;
constexpr KernelQuat kIdentityRotation{0.0f, 0.0f, 0.0f, 1.0f};
constexpr std::uint16_t kPort = 8471;
// A jump bigger than this between two drawn frames is a snap; anything
// between this and a walking step is a slide.
constexpr float kSnapMeters = 2.0f;
// Close enough to the server's position to call the drawing caught up. Not
// tighter: a remote body is drawn ~100 ms in the past, which for a player
// walking at 5 m/s is half a metre behind wherever the server has it now.
constexpr float kCaughtUpMeters = kSnapMeters;

std::vector<std::uint8_t> read_ground_scene() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    const char* test_workspace = std::getenv("TEST_WORKSPACE");
    require(test_srcdir != nullptr);
    require(test_workspace != nullptr);
    const std::filesystem::path path = std::filesystem::path(test_srcdir) /
        test_workspace / "game_server" / "gameplay_catalog" / "mesh_assets" /
        "jolt" / "plane_200x200.joltmesh";
    std::ifstream file(path, std::ios::binary);
    require(file.good());
    return std::vector<std::uint8_t>(
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>());
}

std::uint32_t entity_template_id_of(
    const GameServerGameplayConfig& config, const std::string& name) {
    for (const auto& candidate : config.entity_templates) {
        if (candidate.name == name) {
            return candidate.actor_template_id;
        }
    }
    return 0;
}

KernelHandle* make_engine(
    KernelMode mode,
    const GameServerGameplayConfig& config,
    const std::vector<std::uint8_t>& scene) {
    KernelConfig kernel_config{};
    kernel_config.mode = mode;
    kernel_config.tick.server_tick_rate = 30;
    kernel_config.tick.snapshot_rate = 30;
    kernel_config.max_events = 256;
    kernel_config.max_render_states = 256;
    KernelHandle* kernel = Kernel_Create(&kernel_config);
    require(kernel != nullptr);
    require(network_example::game_server::load_kernel_gameplay_catalog(
        kernel, config));
    return kernel;
}

// A server registers its ground before it starts, a client after it connects
// and before its Welcome.
void set_ground(
    KernelHandle* kernel,
    const GameServerGameplayConfig& config,
    const std::vector<std::uint8_t>& scene) {
    KernelStaticCollisionSceneConfig scene_config{};
    scene_config.struct_size = sizeof(scene_config);
    scene_config.artifact_bytes = scene.data();
    scene_config.artifact_size = static_cast<std::uint32_t>(scene.size());
    scene_config.scene_id = config.static_collision_scene.scene_id;
    scene_config.collider_id = config.static_collision_scene.collider_id;
    scene_config.collision_layer = config.static_collision_scene.collision_layer;
    require(Kernel_SetStaticCollisionScene(kernel, &scene_config));
}

struct Session {
    KernelHandle* server = nullptr;
    KernelHandle* traveller = nullptr;
    KernelHandle* observer = nullptr;
    std::uint32_t traveller_id = 0;
    std::uint32_t observer_id = 0;
    // The server's game_server, without preload_directors(): players are set
    // up as in a real session, and no mission puts enemies on the map.
    network_example::game_server::GameServer* game_server = nullptr;
    // What the traveller is asked to do each frame.
    KernelVec2 traveller_move{0.0f, 0.0f};
    // ActorTeleported events each client has polled.
    std::vector<KernelEvent> traveller_teleports;
    std::vector<KernelEvent> observer_teleports;

    void drain(KernelHandle* kernel, bool to_game_server,
               std::vector<KernelEvent>* teleports = nullptr) {
        std::array<KernelEvent, 256> events{};
        const std::uint32_t count = Kernel_PollEvents(
            kernel, events.data(), static_cast<std::uint32_t>(events.size()));
        for (std::uint32_t index = 0; index < count; ++index) {
            if (to_game_server) {
                game_server->handle_event(events[index]);
            }
            if (teleports != nullptr &&
                events[index].type == KernelEventType_ActorTeleported) {
                teleports->push_back(events[index]);
            }
        }
    }

    void frame() {
        KernelPlayerInput input{};
        input.move = traveller_move;
        input.aim_dir = KernelVec3{1.0f, 0.0f, 0.0f};
        Kernel_SubmitPlayerInput(traveller, 0, &input);
        const KernelPlayerInput idle{};
        Kernel_SubmitPlayerInput(observer, 0, &idle);
        Kernel_Update(traveller, kTickSeconds);
        Kernel_Update(observer, kTickSeconds);
        game_server->tick(kTickSeconds);
        Kernel_Update(server, kTickSeconds);
        drain(server, true);
        drain(traveller, false, &traveller_teleports);
        drain(observer, false, &observer_teleports);
        // Lets localhost deliver what the server just sent.
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // One server tick drawn at 60 fps, as Unity draws it: the clients update
    // twice, half a tick each, and `sample` runs after each -- so a blend
    // across two snapshots shows up halfway rather than landing on one.
    template <typename Sample>
    void frame_at_60fps(Sample&& sample) {
        for (int half = 0; half < 2; ++half) {
            KernelPlayerInput input{};
            input.move = traveller_move;
            input.aim_dir = KernelVec3{1.0f, 0.0f, 0.0f};
            Kernel_SubmitPlayerInput(traveller, 0, &input);
            const KernelPlayerInput idle{};
            Kernel_SubmitPlayerInput(observer, 0, &idle);
            Kernel_Update(traveller, kTickSeconds * 0.5f);
            Kernel_Update(observer, kTickSeconds * 0.5f);
            if (half == 1) {
                game_server->tick(kTickSeconds);
                Kernel_Update(server, kTickSeconds);
                drain(server, true);
            }
            drain(traveller, false, &traveller_teleports);
            drain(observer, false, &observer_teleports);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            sample();
        }
    }

    void frames(int count) {
        for (int index = 0; index < count; ++index) {
            frame();
        }
    }

    KernelVec3 server_position(std::uint32_t net_id) const {
        KernelServerEntityState state{};
        state.struct_size = sizeof(state);
        require(Kernel_ServerGetEntityState(server, net_id, &state));
        return state.position;
    }

    void teleport(std::uint32_t net_id, const KernelVec3& to) const {
        require(Kernel_ServerSetEntityTransform(server, net_id, &to, &kIdentityRotation));
    }
};

std::optional<KernelVec3> drawn(KernelHandle* client, std::uint32_t net_id) {
    std::array<RenderEntityState, 256> states{};
    const std::uint32_t count = Kernel_GetRenderStates(
        client, states.data(), static_cast<std::uint32_t>(states.size()));
    for (std::uint32_t index = 0; index < count; ++index) {
        if (states[index].net_id == net_id) {
            return states[index].position;
        }
    }
    return std::nullopt;
}

float distance(const KernelVec3& a, const KernelVec3& b) {
    const float x = a.x - b.x;
    const float y = a.y - b.y;
    const float z = a.z - b.z;
    return std::sqrt(x * x + y * y + z * z);
}

// Horizontal distance from p to the segment a-b: how far a drawn point is from
// the straight line a slide would follow.
float off_segment(const KernelVec3& p, const KernelVec3& a, const KernelVec3& b) {
    const float abx = b.x - a.x;
    const float abz = b.z - a.z;
    const float length_squared = abx * abx + abz * abz;
    float t = length_squared > 0.0f
        ? ((p.x - a.x) * abx + (p.z - a.z) * abz) / length_squared
        : 0.0f;
    t = std::fmax(0.0f, std::fmin(1.0f, t));
    const float dx = p.x - (a.x + abx * t);
    const float dz = p.z - (a.z + abz * t);
    return std::sqrt(dx * dx + dz * dz);
}

// What one client drew of one body across a teleport from `from` to `to`.
struct Track {
    int frames_until_caught_up = -1;
    int snaps = 0;
    // Frames drawn more than kSnapMeters from both ends, on the line between
    // them: what a slide looks like.
    int in_between = 0;
    float largest_step = 0.0f;
    int frames_missing = 0;
    // Where the in-between frames were drawn, and how far behind the server
    // the drawing still is on the last frame.
    std::vector<KernelVec3> in_between_at;
    float final_lag = 0.0f;
};

void print_track(const char* what, const Track& track) {
    std::printf(
        "%s: caught up after %d frames, %d snap(s), largest step %.2f m, "
        "%d frame(s) drawn in between, %d frame(s) not drawn, final lag %.2f m\n",
        what, track.frames_until_caught_up, track.snaps, track.largest_step,
        track.in_between, track.frames_missing, track.final_lag);
    for (const KernelVec3& at : track.in_between_at) {
        std::printf("    drawn in between at (%.2f, %.2f, %.2f)\n", at.x, at.y, at.z);
    }
}

// Teleports the traveller and follows how `viewer` draws it, frame by frame.
Track follow_teleport(
    Session& session,
    KernelHandle* viewer,
    const KernelVec3& to,
    int frame_count) {
    std::optional<KernelVec3> previous = drawn(viewer, session.traveller_id);
    require(previous.has_value());
    const KernelVec3 from = session.server_position(session.traveller_id);
    session.teleport(session.traveller_id, to);
    Track track;
    for (int index = 1; index <= frame_count; ++index) {
        session.frame();
        const KernelVec3 truth = session.server_position(session.traveller_id);
        const std::optional<KernelVec3> now = drawn(viewer, session.traveller_id);
        if (!now.has_value()) {
            ++track.frames_missing;
            continue;
        }
        const float step = distance(*now, *previous);
        track.largest_step = std::fmax(track.largest_step, step);
        if (step > kSnapMeters) {
            ++track.snaps;
        }
        if (distance(*now, from) > kSnapMeters && distance(*now, truth) > kSnapMeters &&
            off_segment(*now, from, truth) < kSnapMeters) {
            ++track.in_between;
            track.in_between_at.push_back(*now);
        }
        if (track.frames_until_caught_up < 0 &&
            distance(*now, truth) < kCaughtUpMeters) {
            track.frames_until_caught_up = index;
        }
        previous = now;
        track.final_lag = distance(*now, truth);
    }
    return track;
}

// Walks the traveller until the server moves it more than kSnapMeters in one
// tick -- a portal -- and follows how both clients draw it from there, at 60
// fps (frame counts below are 60 fps frames from the start of the walk). A
// frame drawn in between only counts once the jump is known.
struct PortalWalk {
    Track own;
    Track seen;
    bool jumped = false;
};

PortalWalk walk_through_portal(Session& session, int frame_count) {
    PortalWalk walk;
    KernelVec3 from = session.server_position(session.traveller_id);
    std::optional<KernelVec3> previous_own = drawn(session.traveller, session.traveller_id);
    std::optional<KernelVec3> previous_seen = drawn(session.observer, session.traveller_id);
    require(previous_own.has_value() && previous_seen.has_value());
    const auto note = [&](Track* track, const std::optional<KernelVec3>& now,
                          std::optional<KernelVec3>* previous, const KernelVec3& truth,
                          int index) {
        if (!now.has_value()) {
            ++track->frames_missing;
            return;
        }
        const float step = distance(*now, **previous);
        track->largest_step = std::fmax(track->largest_step, step);
        if (step > kSnapMeters) {
            ++track->snaps;
        }
        if (walk.jumped && distance(*now, from) > kSnapMeters &&
            distance(*now, truth) > kSnapMeters &&
            off_segment(*now, from, truth) < kSnapMeters) {
            ++track->in_between;
            track->in_between_at.push_back(*now);
        }
        if (walk.jumped && track->frames_until_caught_up < 0 &&
            distance(*now, truth) < kCaughtUpMeters) {
            track->frames_until_caught_up = index;
        }
        *previous = now;
        track->final_lag = distance(*now, truth);
    };
    int frame = 0;
    for (int index = 1; index <= frame_count; ++index) {
        const KernelVec3 before = session.server_position(session.traveller_id);
        session.frame_at_60fps([&]() {
            ++frame;
            const KernelVec3 truth = session.server_position(session.traveller_id);
            if (!walk.jumped && distance(truth, before) > kSnapMeters) {
                walk.jumped = true;
                from = before;
                // Arrived: stop walking, so "caught up" is about the jump.
                session.traveller_move = KernelVec2{0.0f, 0.0f};
            }
            note(&walk.own, drawn(session.traveller, session.traveller_id),
                 &previous_own, truth, frame);
            note(&walk.seen, drawn(session.observer, session.traveller_id),
                 &previous_seen, truth, frame);
        });
    }
    return walk;
}

int frames_until_drawn(Session& session, KernelHandle* client, std::uint32_t net_id, int limit) {
    for (int index = 1; index <= limit; ++index) {
        session.frame();
        if (drawn(client, net_id).has_value()) {
            return index;
        }
    }
    return -1;
}

}  // namespace

int main() {
    const GameServerGameplayConfig config =
        network_example::game_server::default_game_server_gameplay_config();
    const std::vector<std::uint8_t> scene = read_ground_scene();

    Session session;
    session.server = make_engine(KernelMode_DedicatedServer, config, scene);
    set_ground(session.server, config, scene);
    KernelSessionRulesConfig rules{};
    rules.struct_size = sizeof(rules);
    rules.actor_blocking_mode = KernelActorBlockingMode_Predicted;
    require(Kernel_SetSessionRules(session.server, &rules));
    // No mission: nothing else on the map moves.
    GameServerGameplayConfig quiet = config;
    quiet.game_rules.clear();
    network_example::game_server::GameServer game_server(session.server, quiet);
    session.game_server = &game_server;
    require(Kernel_StartDedicatedServer(session.server, kPort));

    const std::string address = "127.0.0.1:" + std::to_string(kPort);
    session.traveller = make_engine(KernelMode_Client, config, scene);
    session.observer = make_engine(KernelMode_Client, config, scene);
    require(Kernel_StartClient(session.traveller, address.c_str()));
    require(Kernel_StartClient(session.observer, address.c_str()));
    set_ground(session.traveller, config, scene);
    set_ground(session.observer, config, scene);

    KernelLocalPlayerInfo traveller_info{};
    KernelLocalPlayerInfo observer_info{};
    for (int attempt = 0; attempt < 600; ++attempt) {
        session.frame();
        require(Kernel_GetLocalPlayerInfo(session.traveller, &traveller_info));
        require(Kernel_GetLocalPlayerInfo(session.observer, &observer_info));
        if (traveller_info.has_welcome != 0u && observer_info.has_welcome != 0u) {
            break;
        }
    }
    require(traveller_info.has_welcome != 0u);
    require(observer_info.has_welcome != 0u);
    session.traveller_id = traveller_info.player_net_id;
    session.observer_id = observer_info.player_net_id;
    require(session.traveller_id != session.observer_id);

    // Apart, on the ground, and settled.
    session.teleport(session.traveller_id, KernelVec3{-20.0f, 0.5f, 0.0f});
    session.teleport(session.observer_id, KernelVec3{0.0f, 0.5f, 10.0f});
    session.frames(90);
    require(drawn(session.traveller, session.traveller_id).has_value());
    require(drawn(session.observer, session.traveller_id).has_value());

    // Control: walking draws steps, not snaps, and the observer sees it walk.
    session.traveller_move = KernelVec2{1.0f, 0.0f};
    {
        const KernelVec3 start = session.server_position(session.traveller_id);
        std::optional<KernelVec3> previous = drawn(session.traveller, session.traveller_id);
        float largest_step = 0.0f;
        for (int index = 0; index < 30; ++index) {
            session.frame();
            const std::optional<KernelVec3> now = drawn(session.traveller, session.traveller_id);
            require(now.has_value());
            largest_step = std::fmax(largest_step, distance(*now, *previous));
            previous = now;
        }
        const float walked =
            distance(session.server_position(session.traveller_id), start);
        std::printf("control: walked %.2f m in 30 frames, largest drawn step %.3f m\n",
                    walked, largest_step);
        require(walked > 1.0f);
        require(largest_step < kSnapMeters);
    }

    // R1 + R2: 30 m, still walking, ending inside the observer's relevance.
    {
        const KernelVec3 near_end{0.0f, 0.5f, 30.0f};
        // Two runs of the same jump, one watched by each client: drawn() reads
        // a client's frame, and each run needs its own before-and-after.
        const Track own = follow_teleport(session, session.traveller, near_end, 45);
        print_track("R1 traveller draws itself", own);
        require(Kernel_GetLocalPlayerInfo(session.traveller, &traveller_info));
        std::printf("R1 traveller still welcomed (prediction alive): %u\n",
                    traveller_info.has_welcome);
        require(own.frames_until_caught_up > 0);

        session.teleport(session.traveller_id, KernelVec3{-20.0f, 0.5f, 0.0f});
        session.frames(60);
        const Track seen = follow_teleport(session, session.observer, near_end, 45);
        print_track("R2 observer draws the traveller", seen);
        require(seen.frames_until_caught_up > 0);
    }

    // R3: past the relevance radius, onto the observer and two tents.
    session.traveller_move = KernelVec2{0.0f, 0.0f};
    {
        const KernelVec3 far_end{80.0f, 0.5f, 80.0f};
        session.teleport(session.observer_id, KernelVec3{84.0f, 0.5f, 80.0f});
        const std::uint32_t tent = entity_template_id_of(config, "tent");
        require(tent != 0u);
        std::array<std::uint32_t, 2> tents{};
        for (std::size_t index = 0; index < tents.size(); ++index) {
            KernelServerEntityCreateInfo create{};
            create.struct_size = sizeof(create);
            create.entity_template_id = tent;
            create.owner_peer = 1u;
            create.position = KernelVec3{76.0f, 0.0f, 84.0f + 6.0f * index};
            create.rotation = kIdentityRotation;
            require(Kernel_ServerCreateEntity(session.server, &create, &tents[index]));
        }
        session.frames(60);
        // Control: none of it is relevant to the traveller yet.
        require(!drawn(session.traveller, session.observer_id).has_value());
        require(!drawn(session.traveller, tents[0]).has_value());

        session.teleport(session.traveller_id, far_end);
        const int observer_frames =
            frames_until_drawn(session, session.traveller, session.observer_id, 120);
        int tent_frames = observer_frames;
        for (const std::uint32_t tent_id : tents) {
            if (!drawn(session.traveller, tent_id).has_value()) {
                const int more = frames_until_drawn(session, session.traveller, tent_id, 120);
                tent_frames = more < 0 ? -1 : observer_frames + more;
            }
        }
        std::printf("R3 after a %.0f m jump: observer drawn after %d frames, "
                    "both tents after %d frames\n",
                    distance(KernelVec3{0.0f, 0.5f, 30.0f}, far_end),
                    observer_frames, tent_frames);
        require(observer_frames > 0);
        require(tent_frames > 0);
    }

    // R2': through a real portal, the observer seeing both ends.
    {
        const std::uint32_t entrance_template =
            entity_template_id_of(config, "portal_entrance");
        require(entrance_template != 0u);
        const KernelVec3 entrance_at{60.0f, 0.0f, 75.0f};
        session.teleport(session.observer_id, KernelVec3{55.0f, 0.5f, 62.0f});
        session.teleport(session.traveller_id, KernelVec3{60.0f, 0.5f, 72.0f});
        KernelServerEntityCreateInfo create{};
        create.struct_size = sizeof(create);
        create.entity_template_id = entrance_template;
        create.owner_peer = 1u;
        create.position = entrance_at;
        create.rotation = kIdentityRotation;
        std::uint32_t entrance = 0;
        require(Kernel_ServerCreateEntity(session.server, &create, &entrance));
        session.frames(60);
        session.traveller_teleports.clear();
        session.observer_teleports.clear();
        // Its gate faces -z: walk +z into it.
        session.traveller_move = KernelVec2{0.0f, 1.0f};
        const PortalWalk walk = walk_through_portal(session, 60);
        require(walk.jumped);
        print_track("R2' traveller draws itself through a portal", walk.own);
        print_track("R2' observer draws the traveller through a portal", walk.seen);
        std::printf("R2' events: traveller %zu, observer %zu\n",
                    session.traveller_teleports.size(),
                    session.observer_teleports.size());
        for (const auto* events :
             {&session.traveller_teleports, &session.observer_teleports}) {
            require(events->size() == 1u);
            require(events->front().net_id == session.traveller_id);
            require(events->front().code == entrance_template);
            require(events->front().related_net_id == entrance);
        }
        // What a client turns its camera to: the portal's forward, read
        // from its own catalog by the template id the event named.
        KernelPortalDefinition portal{};
        portal.struct_size = sizeof(portal);
        require(Kernel_GetPortalDefinition(
            session.traveller, session.traveller_teleports.front().code, &portal));
        std::printf("R2' portal forward (%.1f, %.1f, %.1f)\n",
                    portal.forward.x, portal.forward.y, portal.forward.z);
        require(portal.forward.z > 0.99f);
        require(walk.own.in_between == 0);
        require(walk.seen.in_between == 0);
        require(walk.seen.frames_until_caught_up > 0);
    }

    Kernel_Destroy(session.observer);
    Kernel_Destroy(session.traveller);
    Kernel_Destroy(session.server);
    std::printf("portal_feasibility_probe_test: done\n");
    return 0;
}
