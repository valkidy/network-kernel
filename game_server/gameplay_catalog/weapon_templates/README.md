# Weapon templates

File name: `<id>_weapon_<name>.yaml`.

Weapon ids:

- **0–127**: player weapons.
- **128–254**: every other weapon (agents, sentries, giants, sp_attack contact weapons).
- **255**: reserved, `KERNEL_HELD_WEAPON_NONE` (an unarmed player).

The loader does not enforce the split; it is a convention. A weapon id is one byte end to end
(the loader's range check, `kWeaponIdCount`, the snapshot's held weapon, the AI's `UINT8_MAX`
checks). Once there are more than 255 weapons, the id must be widened and every weapon id
re-assigned, which bumps the snapshot schema and the ABI.

`category` is the weapon's group (0–3), which is also its weapon-container slot.
