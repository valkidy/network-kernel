---
name: unity-plugin-plan-guideline
description: Use when planning Unity plugin work for network-example that may depend on native C++ kernel, public ABI, version constants, or main branch changes.
---

# Unity Plugin Plan Guideline

Use this skill only while planning Unity plugin work that may touch or depend on the native network kernel. Keep the plan focused on branch flow, native ABI alignment, and Unity package implementation order.

## Planning Workflow

1. Record the starting branch and inspect worktree status before planning any branch switch.
2. Compare `main` against the starting branch for C++ kernel, public ABI, native plugin, and version differences. Pay particular attention to:
   - `engine/src/kernel/public/kernel_api.h`
   - `engine/src/kernel/public/kernel_types.h`
   - `engine/src/kernel/BUILD.bazel`
   - `docs/NETWORK_KERNEL_ABI.md`
   - Unity-side ABI constants and P/Invoke bindings under `plugins/com.network-example.kernel/Runtime/`
3. Decide whether the requested Unity plugin work needs native C++ changes.
4. If native C++ changes are needed, plan the native work first:
   - switch to `main`
   - implement the C++ kernel/API/ABI changes on `main`
   - update ABI/version documentation when the public boundary changes
   - verify the native build/tests with the `bazel-build` skill
   - commit the native work on `main`
   - switch back to the recorded Unity plugin branch
   - merge the new `main` commit and resolve any conflicts before continuing
5. If native C++ changes are not needed, plan to continue directly on the recorded Unity plugin branch.
6. After the native baseline is settled, plan the Unity plugin work under `plugins/com.network-example.kernel/`, keeping C# bindings, ABI constants, samples, editor smoke tools, package assets, and managed smoke tests aligned with the native ABI.

## Planning Cost Control

When producing a Unity plugin implementation plan, include:

- In scope.
- Out of scope.
- File budget.
- Search budget.
- Edit budget.
- Validation budget.
- Deferred follow-ups.

Do not combine unrelated phases unless explicitly requested.

Avoid combining all of the following in one phase:

- Bazel target changes.
- Package builder changes.
- Windows regression.
- Docs updates.
- Broad final validation.

## Plan Requirements

- State the starting branch and the Unity plugin branch the agent should return to.
- State whether native C++ work is required, and why.
- Include the exact branch sequence for the chosen path.
- Include native verification when C++ or Bazel files change.
- Include Unity-side validation for C# bindings, samples, editor scripts, or managed ABI smoke tests when those surfaces change.
- Do not plan to change repo history destructively. If branch state is dirty or ambiguous, plan to stop and ask before switching branches.
