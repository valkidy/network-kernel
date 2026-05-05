# Trigger Conditions

Activate when:
- Creating or modifying C++ files under `engine/`
- Editing Bazel files, `.bazelrc`, `WORKSPACE`, or `platform_mappings`
- Adding or changing `third_party/*.BUILD` wrappers
- Adding platform-specific logic or platform mapping entries
- Refactoring the repository layout

Do NOT trigger for:
- small bug fixes
- documentation-only changes
- unrelated skill text edits that do not affect C++/Bazel conventions
