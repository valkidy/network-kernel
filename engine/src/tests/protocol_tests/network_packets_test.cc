#include <cassert>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "protocol/public/network_packets.h"

namespace {

bool nearly_equal(float lhs, float rhs) {
    const float diff = lhs > rhs ? lhs - rhs : rhs - lhs;
    return diff < 0.0001f;
}

}  // namespace

int main() {
    KernelPlayerInput input{};
    input.input_seq = 7;
    input.client_action_time_us = 11000;
    input.move = KernelVec2{0.5f, -0.25f};
    input.aim_dir = KernelVec3{1.0f, 0.0f, 0.0f};
    input.buttons = InputButton_Sprint | InputButton_Dodge | InputButton_Parry;
    input.selected_weapon = 2;
    input.action_intent = KernelActionIntent{
        1234u, KernelActionBinding_PrimaryFire, 0u, 0u};
    input.action_input = KernelActionInput{1234u, 1u, 0u, 0u};

    const std::vector<std::uint8_t> input_packet =
        network_example::encode_player_input_packet(3, input, 42);
    assert(input_packet.size() == 85u);
    network_example::PeerId decoded_player = 0;
    KernelPlayerInput decoded_input{};
    assert(network_example::decode_player_input_packet(
        input_packet.data(),
        input_packet.size(),
        &decoded_player,
        &decoded_input));
    assert(decoded_player == 3);
    assert(decoded_input.input_seq == input.input_seq);
    assert(decoded_input.client_action_time_us == input.client_action_time_us);
    assert(decoded_input.action_intent.action_instance_id ==
           input.action_intent.action_instance_id);
    assert(decoded_input.action_intent.binding_id ==
           input.action_intent.binding_id);
    assert(decoded_input.action_input.action_instance_id ==
           input.action_input.action_instance_id);
    assert(decoded_input.action_input.held == input.action_input.held);
    assert(nearly_equal(decoded_input.move.x, input.move.x));
    assert(nearly_equal(decoded_input.move.y, input.move.y));
    assert(nearly_equal(decoded_input.aim_dir.x, input.aim_dir.x));
    assert(decoded_input.buttons == input.buttons);
    assert(decoded_input.selected_weapon == input.selected_weapon);

    network_example::WorldSnapshot snapshot;
    snapshot.header.server_tick = 9;
    snapshot.header.server_time_ms = 300;
    snapshot.header.last_processed_input_seq = 7;
    network_example::EntitySnapshot player;
    player.net_id = 4;
    player.type = network_example::EntityType::kActor;
    player.actor_type = network_example::ActorType::kPlayer;
    player.owner_peer = 3;
    player.position = glm::vec3{0.0f, 1.0f, 2.0f};
    player.rotation = glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
    player.velocity = glm::vec3{0.5f, 0.0f, 0.0f};
    player.hp = 88;
    player.max_hp = 120;
    player.state = 512;
    player.flags = 0x00000004u;
    player.aim_direction = glm::vec3{0.0f, 0.0f, 1.0f};
    player.action_template_id = 1001;
    player.action_instance_id = 7001;
    player.action_phase = KernelActionPhase_Windup;
    player.action_start_tick = 8;
    player.action_commit_count = 0;
    player.has_authoritative_movement_state = true;
    player.ground_state = 1;
    player.ground_normal = glm::vec3{0.0f, 1.0f, 0.0f};
    player.supporting_entity_net_id = 99;
    player.supporting_collider_id = 123;
    snapshot.entities.push_back(player);
    network_example::EntitySnapshot enemy;
    enemy.net_id = 6;
    enemy.type = network_example::EntityType::kActor;
    enemy.actor_type = network_example::ActorType::kAgent;
    enemy.position = glm::vec3{7.0f, 8.0f, 9.0f};
    enemy.rotation = glm::quat{0.0f, 0.0f, 1.0f, 0.0f};
    enemy.velocity = glm::vec3{1.0f, 0.0f, 0.0f};
    enemy.state = 513;
    enemy.flags = 0x01020304u;
    snapshot.entities.push_back(enemy);
    network_example::EntitySnapshot compact_projectile;
    compact_projectile.net_id = 5;
    compact_projectile.type = network_example::EntityType::kProjectile;
    compact_projectile.position = glm::vec3{1.0f, 2.0f, 3.0f};
    compact_projectile.rotation = glm::quat{0.5f, 0.5f, 0.5f, 0.5f};
    compact_projectile.velocity = glm::vec3{4.0f, 5.0f, 6.0f};
    compact_projectile.state = 514;
    compact_projectile.flags = 0x02030405u;
    snapshot.entities.push_back(compact_projectile);
    network_example::EntitySnapshot hybrid_projectile = compact_projectile;
    hybrid_projectile.net_id = 7;
    hybrid_projectile.owner_peer = 3;
    hybrid_projectile.spawn_tick = 12;
    hybrid_projectile.action_instance_id = 1234;
    hybrid_projectile.state_flags |=
        network_example::kSnapshotStateFlagProjectileHybridCorrection;
    snapshot.entities.push_back(hybrid_projectile);
    network_example::EntitySnapshot item_prop;
    item_prop.net_id = 8;
    item_prop.type = network_example::EntityType::kProp;
    item_prop.item_template_id = 501;
    item_prop.item_instance_id = 9001;
    item_prop.world_item_mode = KernelWorldItemMode_Carrying;
    item_prop.carrier_entity_id = 4;
    item_prop.hp = 50;
    item_prop.max_hp = 100;
    snapshot.entities.push_back(item_prop);

    const std::vector<std::uint8_t> snapshot_packet =
        network_example::encode_snapshot_packet(snapshot, 43);
    assert(network_example::estimate_snapshot_packet_size(snapshot) ==
           snapshot_packet.size());
    assert(network_example::estimate_snapshot_entity_size(player) == 118u);
    network_example::EntitySnapshot owner_without_action = player;
    owner_without_action.action_template_id = 0;
    owner_without_action.action_instance_id = 0;
    owner_without_action.action_phase = KernelActionPhase_None;
    assert(network_example::estimate_snapshot_entity_size(owner_without_action) == 98u);
    assert(network_example::estimate_snapshot_entity_size(enemy) == 68u);
    network_example::EntitySnapshot active_enemy = enemy;
    active_enemy.action_template_id = 1002;
    active_enemy.action_phase = KernelActionPhase_Active;
    assert(network_example::estimate_snapshot_entity_size(active_enemy) == 88u);
    assert(network_example::estimate_snapshot_entity_size(compact_projectile) == 34u);
    assert(network_example::estimate_snapshot_entity_size(hybrid_projectile) == 46u);
    network_example::WorldSnapshot decoded_snapshot;
    assert(network_example::decode_snapshot_packet(
        snapshot_packet.data(),
        snapshot_packet.size(),
        &decoded_snapshot));
    assert(decoded_snapshot.header.server_tick == 9);
    assert(decoded_snapshot.header.server_time_ms == 300);
    assert(decoded_snapshot.header.last_processed_input_seq == 7);
    assert(decoded_snapshot.entities.size() == 5);
    assert(decoded_snapshot.entities[4].item_template_id == 0u);
    assert(decoded_snapshot.entities[4].item_instance_id == 0u);
    assert(decoded_snapshot.entities[4].hp == 50u);
    assert(decoded_snapshot.entities[4].max_hp == 100u);
    assert(decoded_snapshot.entities[0].net_id == 4);
    assert(decoded_snapshot.entities[0].type == network_example::EntityType::kActor);
    assert(decoded_snapshot.entities[0].actor_type ==
           network_example::ActorType::kPlayer);
    assert(decoded_snapshot.entities[0].owner_peer == 3);
    assert(nearly_equal(decoded_snapshot.entities[0].rotation.w, 1.0f));
    assert(decoded_snapshot.entities[0].hp == 88);
    assert(decoded_snapshot.entities[0].max_hp == 120);
    assert(nearly_equal(decoded_snapshot.entities[0].aim_direction.z, 1.0f));
    assert(decoded_snapshot.entities[0].action_template_id == 1001);
    assert(decoded_snapshot.entities[0].action_instance_id == 7001);
    assert(decoded_snapshot.entities[0].action_phase == KernelActionPhase_Windup);
    assert(decoded_snapshot.entities[0].action_start_tick == 8);
    assert(decoded_snapshot.entities[0].has_authoritative_movement_state);
    assert(decoded_snapshot.entities[0].ground_state == 1);
    assert(nearly_equal(decoded_snapshot.entities[0].ground_normal.y, 1.0f));
    assert(decoded_snapshot.entities[0].supporting_entity_net_id == 99);
    assert(decoded_snapshot.entities[0].supporting_collider_id == 123);
    assert((decoded_snapshot.entities[0].state_flags &
            network_example::kSnapshotStateFlagHpUnknown) == 0u);
    assert(decoded_snapshot.entities[1].net_id == 6);
    assert(decoded_snapshot.entities[1].type == network_example::EntityType::kActor);
    assert(decoded_snapshot.entities[1].actor_type ==
           network_example::ActorType::kAgent);
    assert(nearly_equal(decoded_snapshot.entities[1].rotation.y, 1.0f));
    assert((decoded_snapshot.entities[1].state_flags &
            network_example::kSnapshotStateFlagHpUnknown) != 0u);
    assert(decoded_snapshot.entities[1].hp == 0);
    assert(decoded_snapshot.entities[1].max_hp == 0);
    assert(!decoded_snapshot.entities[1].has_authoritative_movement_state);
    assert(decoded_snapshot.entities[2].net_id == 5);
    assert(decoded_snapshot.entities[2].type == network_example::EntityType::kProjectile);
    assert(decoded_snapshot.entities[2].owner_peer == 0);
    assert(nearly_equal(decoded_snapshot.entities[2].position.x, 1.0f));
    assert(!nearly_equal(decoded_snapshot.entities[2].rotation.w, 0.5f));
    assert(nearly_equal(decoded_snapshot.entities[2].velocity.z, 6.0f));
    assert(decoded_snapshot.entities[2].spawn_tick == 0);
    assert(decoded_snapshot.entities[2].action_instance_id == 0);
    assert(decoded_snapshot.entities[3].net_id == 7);
    assert(decoded_snapshot.entities[3].type == network_example::EntityType::kProjectile);
    assert(decoded_snapshot.entities[3].owner_peer == 3);
    assert(decoded_snapshot.entities[3].spawn_tick == 12);
    assert(decoded_snapshot.entities[3].action_instance_id == 1234);
    assert((decoded_snapshot.entities[3].state_flags &
            network_example::kSnapshotStateFlagProjectileHybridCorrection) != 0u);
    assert(decoded_snapshot.entities[4].net_id == 8);
    assert(decoded_snapshot.entities[4].item_template_id == 501);
    assert(decoded_snapshot.entities[4].item_instance_id == 9001);
    assert(decoded_snapshot.entities[4].world_item_mode ==
           KernelWorldItemMode_Carrying);
    assert(decoded_snapshot.entities[4].carrier_entity_id == 4);

    KernelEvent reliable_event{};
    reliable_event.type = KernelEventType_PlayerLeft;
    reliable_event.tick = 19;
    reliable_event.net_id = 23;
    reliable_event.peer_id = 4;
    reliable_event.code = 99;
    reliable_event.event_time_us = 123456;
    reliable_event.presentation_time_us = 234567;
    const std::vector<std::uint8_t> reliable_event_packet =
        network_example::encode_reliable_event_packet(reliable_event, 44);
    KernelEvent decoded_event{};
    assert(network_example::decode_reliable_event_packet(
        reliable_event_packet.data(),
        reliable_event_packet.size(),
        &decoded_event));
    assert(decoded_event.type == KernelEventType_PlayerLeft);
    assert(decoded_event.tick == 19);
    assert(decoded_event.net_id == 23);
    assert(decoded_event.peer_id == 4);
    assert(decoded_event.code == 99);
    assert(decoded_event.event_time_us == 123456);
    assert(decoded_event.presentation_time_us == 234567);
    assert(!network_example::decode_reliable_event_packet(
        input_packet.data(),
        input_packet.size(),
        &decoded_event));

    network_example::EntitySpawnPacket spawn{};
    spawn.net_id = 41;
    spawn.entity_type = network_example::EntityType::kActor;
    spawn.actor_type = network_example::ActorType::kAgent;
    spawn.owner_peer = 9;
    spawn.server_tick = 12;
    spawn.actor_template_id = 2;
    spawn.position = glm::vec3{3.0f, 4.0f, 5.0f};
    spawn.rotation = glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
    spawn.entity_template_id = 200;
    spawn.collider_template_id = 300;
    spawn.item_template_id = 501;
    spawn.item_instance_id = 9001;
    spawn.world_item_mode = KernelWorldItemMode_Carrying;
    spawn.carrier_entity_id = 4;
    const std::vector<std::uint8_t> spawn_packet =
        network_example::encode_entity_spawn_packet(spawn, 45);
    assert(spawn_packet.size() == 101u);
    network_example::EntitySpawnPacket decoded_spawn{};
    assert(network_example::decode_entity_spawn_packet(
        spawn_packet.data(),
        spawn_packet.size(),
        &decoded_spawn));
    assert(decoded_spawn.net_id == 41);
    assert(decoded_spawn.entity_type == network_example::EntityType::kActor);
    assert(decoded_spawn.actor_type == network_example::ActorType::kAgent);
    assert(decoded_spawn.owner_peer == 9);
    assert(decoded_spawn.server_tick == 12);
    assert(decoded_spawn.actor_template_id == 2);
    assert(decoded_spawn.entity_template_id == 200);
    assert(decoded_spawn.collider_template_id == 300);
    assert(decoded_spawn.item_template_id == 501);
    assert(decoded_spawn.item_instance_id == 9001);
    assert(decoded_spawn.world_item_mode == KernelWorldItemMode_Carrying);
    assert(decoded_spawn.carrier_entity_id == 4);
    assert(nearly_equal(decoded_spawn.position.y, 4.0f));
    assert(nearly_equal(decoded_spawn.rotation.w, 1.0f));
    assert(!network_example::decode_entity_spawn_packet(
        reliable_event_packet.data(),
        reliable_event_packet.size(),
        &decoded_spawn));

    network_example::EntityDespawnPacket despawn{};
    despawn.net_id = 41;
    despawn.server_tick = 18;
    despawn.reason = KernelDespawnReason_OutOfRange;
    const std::vector<std::uint8_t> despawn_packet =
        network_example::encode_entity_despawn_packet(despawn, 46);
    network_example::EntityDespawnPacket decoded_despawn{};
    assert(network_example::decode_entity_despawn_packet(
        despawn_packet.data(),
        despawn_packet.size(),
        &decoded_despawn));
    assert(decoded_despawn.net_id == 41);
    assert(decoded_despawn.server_tick == 18);
    assert(decoded_despawn.reason == KernelDespawnReason_OutOfRange);

    network_example::EntityTemplateUpdatePacket template_update{};
    template_update.net_id = 41;
    template_update.server_tick = 21;
    template_update.actor_template_id = 2;
    const std::vector<std::uint8_t> template_update_packet =
        network_example::encode_entity_template_update_packet(template_update, 49);
    network_example::EntityTemplateUpdatePacket decoded_template_update{};
    assert(network_example::decode_entity_template_update_packet(
        template_update_packet.data(),
        template_update_packet.size(),
        &decoded_template_update));
    assert(decoded_template_update.net_id == 41);
    assert(decoded_template_update.server_tick == 21);
    assert(decoded_template_update.actor_template_id == 2);
    assert(!network_example::decode_entity_template_update_packet(
        despawn_packet.data(),
        despawn_packet.size(),
        &decoded_template_update));

    network_example::ProjectileSpawnBatchPacket batch{};
    batch.server_tick = 77;
    batch.server_time_us = 77000;
    batch.catalog_hash = 0x8877665544332211ull;
    network_example::ProjectileSpawnGroup group{};
    group.projectile_template_id = 3;
    group.records.push_back(network_example::ProjectileSpawnRecord{
        101,
        11,
        7,
        1234,
        glm::vec3{1.0f, 2.0f, 3.0f},
        glm::vec3{4.0f, 5.0f, 6.0f},
    });
    batch.groups.push_back(group);
    const std::vector<std::uint8_t> batch_packet =
        network_example::encode_projectile_spawn_batch_packet(batch, 47);
    network_example::ProjectileSpawnBatchPacket decoded_batch{};
    assert(network_example::decode_projectile_spawn_batch_packet(
        batch_packet.data(),
        batch_packet.size(),
        &decoded_batch));
    assert(decoded_batch.server_tick == 77);
    assert(decoded_batch.server_time_us == 77000);
    assert(decoded_batch.catalog_hash == 0x8877665544332211ull);
    assert(decoded_batch.groups.size() == 1);
    assert(decoded_batch.groups[0].projectile_template_id == 3);
    assert(decoded_batch.groups[0].records.size() == 1);
    assert(decoded_batch.groups[0].records[0].projectile_net_id == 101);
    assert(decoded_batch.groups[0].records[0].owner_net_id == 11);
    assert(decoded_batch.groups[0].records[0].owner_peer == 7);
    assert(decoded_batch.groups[0].records[0].action_instance_id == 1234);
    assert(nearly_equal(decoded_batch.groups[0].records[0].spawn_position.y, 2.0f));
    assert(nearly_equal(decoded_batch.groups[0].records[0].initial_velocity.z, 6.0f));

    std::vector<std::uint8_t> bad_batch_crc = batch_packet;
    bad_batch_crc.back() ^= 0xffu;
    assert(!network_example::decode_projectile_spawn_batch_packet(
        bad_batch_crc.data(),
        bad_batch_crc.size(),
        &decoded_batch));

    network_example::LocalActionResultBatchPacket local_results{};
    local_results.server_tick = 44;
    local_results.records.push_back(KernelLocalActionResult{
        7001,
        2,
        KernelLocalActionResultType_Corrected,
        KernelLocalActionResultReason_Cancelled,
        43,
    });
    const std::vector<std::uint8_t> local_result_packet =
        network_example::encode_local_action_result_batch_packet(
            local_results,
            21);
    assert(local_result_packet.size() == 28u + 8u + 12u);
    network_example::LocalActionResultBatchPacket decoded_local_results{};
    assert(network_example::decode_local_action_result_batch_packet(
        local_result_packet.data(),
        local_result_packet.size(),
        &decoded_local_results));
    assert(decoded_local_results.server_tick == 44);
    assert(decoded_local_results.records.size() == 1);
    assert(decoded_local_results.records[0].action_instance_id == 7001);
    assert(decoded_local_results.records[0].confirmed_commit_count == 2);
    assert(decoded_local_results.records[0].result ==
           KernelLocalActionResultType_Corrected);
    local_results.records.resize(97u);
    assert(network_example::encode_local_action_result_batch_packet(
               local_results, 22).size() == 1200u);
    local_results.records.resize(98u);
    assert(network_example::encode_local_action_result_batch_packet(
               local_results, 23).size() == 1212u);

    network_example::RemoteActionPresentationBatchPacket presentation{};
    presentation.server_tick = 50;
    presentation.records.push_back(KernelRemoteActionPresentationEvent{
        101,
        9,
        7001,
        1,
        3,
        KernelRemoteActionPresentationEventType_FireCommit,
        0,
        2,
    });
    const std::vector<std::uint8_t> presentation_packet =
        network_example::encode_remote_action_presentation_batch_packet(
            presentation,
            22);
    assert(presentation_packet.size() == 28u + 8u + 20u);
    network_example::RemoteActionPresentationBatchPacket decoded_presentation{};
    assert(network_example::decode_remote_action_presentation_batch_packet(
        presentation_packet.data(),
        presentation_packet.size(),
        &decoded_presentation));
    assert(decoded_presentation.records.size() == 1);
    assert(decoded_presentation.records[0].actor_net_id == 101);
    assert(decoded_presentation.records[0].commit_count == 3);
    assert(decoded_presentation.records[0].server_tick_delta == 2);
    presentation.records.resize(58u);
    assert(network_example::encode_remote_action_presentation_batch_packet(
               presentation, 24).size() == 1196u);
    presentation.records.resize(59u);
    assert(network_example::encode_remote_action_presentation_batch_packet(
               presentation, 25).size() == 1216u);

    std::vector<std::uint8_t> bad_presentation_count = presentation_packet;
    bad_presentation_count[32] = 2u;
    assert(!network_example::decode_remote_action_presentation_batch_packet(
        bad_presentation_count.data(),
        bad_presentation_count.size(),
        &decoded_presentation));

    KernelGameplayRequest gameplay_request{};
    gameplay_request.struct_size = sizeof(gameplay_request);
    gameplay_request.requester_peer = 3;
    gameplay_request.request_id = 10001;
    gameplay_request.instigator_net_id = 4;
    gameplay_request.domain_action = KernelDomainAction_Consume;
    gameplay_request.selected_item_instance_id = 9001;
    gameplay_request.target_net_id = 6;
    gameplay_request.requested_quantity = 2;
    gameplay_request.placement_position = KernelVec3{1.0f, 2.0f, 3.0f};
    gameplay_request.throw_direction = KernelVec3{0.0f, 1.0f, 0.0f};
    const std::vector<std::uint8_t> gameplay_request_packet =
        network_example::encode_gameplay_request_packet(gameplay_request, 26);
    KernelGameplayRequest decoded_gameplay_request{};
    assert(network_example::decode_gameplay_request_packet(
        gameplay_request_packet.data(),
        gameplay_request_packet.size(),
        &decoded_gameplay_request));
    assert(decoded_gameplay_request.request_id == 10001);
    assert(decoded_gameplay_request.domain_action == KernelDomainAction_Consume);
    assert(decoded_gameplay_request.selected_item_instance_id == 9001);
    assert(decoded_gameplay_request.requested_quantity == 2);

    KernelGameplayRequestOutcome gameplay_outcome{};
    gameplay_outcome.struct_size = sizeof(gameplay_outcome);
    gameplay_outcome.requester_peer = 3;
    gameplay_outcome.request_id = 10001;
    gameplay_outcome.status = KernelGameplayRequestStatus_Committed;
    gameplay_outcome.graph_outcome = KernelGameplayGraphOutcome_Succeeded;
    gameplay_outcome.domain_action = KernelDomainAction_Consume;
    gameplay_outcome.item_instance_id = 9001;
    gameplay_outcome.committed_quantity = 2;
    const std::vector<std::uint8_t> gameplay_outcome_packet =
        network_example::encode_gameplay_request_outcome_packet(
            gameplay_outcome, 27);
    KernelGameplayRequestOutcome decoded_gameplay_outcome{};
    assert(network_example::decode_gameplay_request_outcome_packet(
        gameplay_outcome_packet.data(),
        gameplay_outcome_packet.size(),
        &decoded_gameplay_outcome));
    assert(decoded_gameplay_outcome.status ==
           KernelGameplayRequestStatus_Committed);
    assert(decoded_gameplay_outcome.graph_outcome ==
           KernelGameplayGraphOutcome_Succeeded);
    assert(decoded_gameplay_outcome.committed_quantity == 2);

    network_example::InventoryDeltaBatchPacket inventory_batch;
    inventory_batch.inventory_container_id = 70;
    inventory_batch.first_revision = 4;
    network_example::InventoryDeltaRecord add;
    add.type = KernelInventoryDeltaType_Add;
    add.slot = 2;
    add.item.item_instance_id = 9001;
    add.item.item_template_id = 501;
    add.item.quantity = 3;
    add.item.next_use_tick = 44;
    add.item.portable_values = {12u, 1u};
    inventory_batch.records.push_back(add);
    network_example::InventoryDeltaRecord remove;
    remove.type = KernelInventoryDeltaType_Remove;
    remove.slot = 3;
    remove.item.item_instance_id = 9002;
    inventory_batch.records.push_back(remove);
    const auto inventory_packet =
        network_example::encode_inventory_delta_batch_packet(inventory_batch, 28);
    assert(inventory_packet.size() == 97u);
    network_example::InventoryDeltaBatchPacket decoded_inventory;
    assert(network_example::decode_inventory_delta_batch_packet(
        inventory_packet.data(), inventory_packet.size(), &decoded_inventory));
    assert(decoded_inventory.first_revision == 4);
    assert(decoded_inventory.records.size() == 2);
    assert(decoded_inventory.records[0].item.portable_values[0] == 12u);

    network_example::InventorySnapshotRequestPacket snapshot_request{70, 3};
    const auto snapshot_request_bytes =
        network_example::encode_inventory_snapshot_request_packet(
            snapshot_request, 29);
    assert(snapshot_request_bytes.size() == 44u);
    network_example::InventorySnapshotRequestPacket decoded_snapshot_request;
    assert(network_example::decode_inventory_snapshot_request_packet(
        snapshot_request_bytes.data(), snapshot_request_bytes.size(),
        &decoded_snapshot_request));
    assert(decoded_snapshot_request.client_revision == 3);

    network_example::InventorySnapshotPagePacket inventory_snapshot;
    inventory_snapshot.inventory_container_id = 70;
    inventory_snapshot.owner_entity_id = 4;
    inventory_snapshot.revision = 5;
    inventory_snapshot.slot_capacity = 8;
    inventory_snapshot.page_count = 1;
    inventory_snapshot.entries.push_back({2, add.item});
    const auto inventory_snapshot_bytes =
        network_example::encode_inventory_snapshot_page_packet(
            inventory_snapshot, 30);
    assert(inventory_snapshot_bytes.size() == 89u);
    network_example::InventorySnapshotPagePacket decoded_inventory_snapshot;
    assert(network_example::decode_inventory_snapshot_page_packet(
        inventory_snapshot_bytes.data(), inventory_snapshot_bytes.size(),
        &decoded_inventory_snapshot));
    assert(decoded_inventory_snapshot.entries.size() == 1);
    assert(decoded_inventory_snapshot.entries[0].slot == 2);

    network_example::PropStateChangeBatchPacket prop_changes;
    prop_changes.server_tick = 50;
    network_example::PropStateChangeRecord prop_change;
    prop_change.net_id = 12;
    prop_change.changed_fields = network_example::kPropStateChangeMode |
        network_example::kPropStateChangeTransform |
        network_example::kPropStateChangeVelocity |
        network_example::kPropStateChangeHealth;
    prop_change.world_mode = KernelWorldItemMode_InFlight;
    prop_change.position = glm::vec3{1.0f, 2.0f, 3.0f};
    prop_change.rotation = glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
    prop_change.velocity = glm::vec3{4.0f, 5.0f, 6.0f};
    prop_change.hp = 3;
    prop_change.max_hp = 5;
    prop_changes.records.push_back(prop_change);
    const auto prop_change_bytes =
        network_example::encode_prop_state_change_batch_packet(prop_changes, 31);
    assert(prop_change_bytes.size() == 88u);
    network_example::PropStateChangeBatchPacket decoded_prop_changes;
    assert(network_example::decode_prop_state_change_batch_packet(
        prop_change_bytes.data(), prop_change_bytes.size(),
        &decoded_prop_changes));
    assert(decoded_prop_changes.records[0].world_mode ==
        KernelWorldItemMode_InFlight);
    assert(decoded_prop_changes.records[0].hp == 3u);
    assert(decoded_prop_changes.records[0].max_hp == 5u);
    network_example::PropStateChangeBatchPacket invalid_prop_health = prop_changes;
    invalid_prop_health.records[0].hp = 6;
    assert(network_example::encode_prop_state_change_batch_packet(
               invalid_prop_health, 32).empty());

    std::vector<std::uint8_t> bad_header = input_packet;
    bad_header[0] = 0;
    assert(!network_example::decode_player_input_packet(
        bad_header.data(),
        bad_header.size(),
        &decoded_player,
        &decoded_input));

    std::vector<std::uint8_t> bad_crc = input_packet;
    bad_crc.back() ^= 0xffu;
    assert(!network_example::decode_player_input_packet(
        bad_crc.data(),
        bad_crc.size(),
        &decoded_player,
        &decoded_input));
    std::vector<std::uint8_t> bad_reliable_crc = reliable_event_packet;
    bad_reliable_crc.back() ^= 0xffu;
    assert(!network_example::decode_reliable_event_packet(
        bad_reliable_crc.data(),
        bad_reliable_crc.size(),
        &decoded_event));

    std::vector<std::uint8_t> bad_size = input_packet;
    bad_size.pop_back();
    assert(!network_example::decode_player_input_packet(
        bad_size.data(),
        bad_size.size(),
        &decoded_player,
        &decoded_input));
    std::vector<std::uint8_t> bad_reliable_size = reliable_event_packet;
    bad_reliable_size.pop_back();
    assert(!network_example::decode_reliable_event_packet(
        bad_reliable_size.data(),
        bad_reliable_size.size(),
        &decoded_event));
    return 0;
}
