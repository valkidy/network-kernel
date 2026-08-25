# Action graph test bottles

A pair of catalog-only vehicles for exercising an action graph in the running
game, without writing a C++ test and without touching the engine. Both are
pure YAML: dropping a file into a template directory *is* the registration, so
adding your own costs one `bazel build` of the bundle and nothing else.

Authoring format and the input contract for every supported trigger and action
live in `ACTION_GRAPH_AUTHORING_AND_RUNTIME_GUIDE.md`. This document is only
about the vehicles.

## What ships

| Vehicle | Item template | Fires on | What it does |
|---|---|---|---|
| `test_bottle_spawn_enemy` | `item_templates/test_bottle_spawn_enemy.yaml` (id 3005) | the thrown prop's `on_collision` | spawns a `chaser_grunt` where it lands, then breaks |
| `test_bottle_blast` | `item_templates/test_bottle_blast.yaml` (id 3006) | `on_item_used` | detonates a `rocket_explosion` at the aim point |

`test_bottle_blast` is worth a note: `rocket_explosion` is an authored
area-effect projectile that already carries the AoE damage and knockback graph
(`projectile_templates/rocket_explosion.yaml` →
`action_rocket_explosion_at_target`). Using the bottle therefore exercises the
entire area-effect path — radius, falloff, `apply_damage` and `apply_impulse` —
end to end, which makes it the cheapest way to eyeball knockback behaviour.

## Getting one in your hands

Neither vehicle is in the player's starting loadout, because
`gameplay_config_test.cc` pins `player_template.inventory_slots.size() == 5`
and adding a slot turns that test red. Two ways in:

- **At runtime, no catalog change.** Both RPCs are `DeveloperWrite`:
  - `item.create_world(item_template_id, quantity, position)` drops a
    pickupable item in the world — walk over it and pick it up.
  - `inventory.create_item(item_template_id, quantity, container_id)` puts one
    straight into a container.
- **Persistently.** Add a slot to `inventory_slots` in
  `entity_templates/player.yaml` (there is spare capacity: 5 of 8 used) **and**
  update the count assertion in `gameplay_config_test.cc`. Do both or the test
  goes red.

## Testing your own graph

The vehicles are deliberately thin — the only interesting lines are the graph
reference and the parameters bound to it.

**A graph that acts on the world where a thrown object lands** — copy
`entity_templates/test_bottle_spawn_enemy_prop.yaml` and
`item_templates/test_bottle_spawn_enemy.yaml`:

1. In the prop file: pick an unused `id` (props currently occupy 200–206), give
   it a `name`, then change `triggers.on_collision.action_graph` to your graph
   and replace the `parameters:` block with your graph's parameters.
2. In the item file: pick an unused `id` (items are 3000–3006), give it a
   `name`, and point `entity_template` at your prop's name.
3. `bazel build //game_server/gameplay_catalog_bundle:bundle.zip`

**A graph that acts where the player aims** — copy
`item_templates/test_bottle_blast.yaml`, change the `id`, the `name`, and
`triggers.on_item_used.action_graph` plus its parameters. No prop template
needed.

`action_graph_templates/action_spawn_ice_and_damage_self_at_collision.yaml` is
worth knowing about: despite the name it is fully generic — it spawns whatever
`template` names and then damages `source`. `test_bottle_spawn_enemy` uses it
unchanged to spawn a `chaser_grunt`. Reach for it before writing a new graph.

The self-damage is not decoration. A prop that survives its own `on_collision`
keeps colliding with whatever it came to rest on, so a vehicle without it
re-fires its graph for as long as it lives. Give the prop `hp: 1` and damage
`self` by 1.

## Constraints you have to design around

These are properties of the current engine, not of the vehicles:

- **`on_collision` has no `event.instigator`.** It provides `event.subject`,
  `event.position`, `event.target` and `event.direction` only, so a graph that
  needs to reach whoever threw the object cannot be tested this way. Use an
  item's `on_item_used` trigger, which does provide it.
- **`spawn_entity` must bind `position` to `event.position`**, and `direction`
  to `event.direction` if it binds it at all.
- **`apply_impulse` must bind `direction`** to `event.direction` or to a vec3
  default on a parameter named `direction`.
- **A spawned agent is picked up by the AI automatically.** `AgentRuntimeManager`
  scans every entity with `actor_type == agent` regardless of who created it,
  so `test_bottle_spawn_enemy` produces a chaser that behaves exactly like a
  director-spawned one.
- **A prop with no `on_collision` binding slides.** Once pushed it travels in a
  straight line until its lifetime expires — see §4.4 of the authoring guide.

## Known gap: `spawn_projectile` on an entity trigger

The natural shape for `test_bottle_blast` would have been a thrown prop whose
`on_collision` spawns the explosion where it lands, matching
`test_bottle_spawn_enemy`. That does not load, and the failure is worth
understanding because it is a loader/kernel disagreement rather than an
authoring mistake:

- The catalog loader accepts `spawn_projectile` on an entity trigger and
  compiles it. `compile_action_trigger_binding` is handed
  `&config.projectile_templates` for `on_collision`
  (`gameplay_config.cc`, the `entity_template.collision_trigger =` call), and
  the `spawn_projectile` branch resolves the template and sets
  `KernelEntityTriggerActionType_SpawnProjectile`.
- The kernel's own entity-template validator inside
  `KernelEngine::load_gameplay_catalog` (`kernel.cc`) has branches for
  `ApplyDamage`, `ApplyHealthChange`, `ApplyImpulse`, `ApplyStatus`,
  `RemoveStatus`, `ApplySpeedModifier` and `SpawnEntity` — and no branch for
  `SpawnProjectile`, so it falls through to `return false`.

The result is that `Kernel_LoadGameplayCatalog` rejects **the whole catalog**,
not just the offending template, and the diagnostic is a bare load failure that
names nothing. Adding the missing branch looks like a small change, but it is
an engine change and has not been made here.
