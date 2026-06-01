## Token / Cost Awareness

When producing a plan, include token/cost awareness as part of the task constraints.

Default rules:
- Prefer smoke-test-first validation.
- Avoid broad repository search.
- Read only directly relevant files.
- Avoid full build/test unless explicitly requested.
- If scope expands, stop and explain why.
- At completion, report files read, files modified, searches, and build/test commands.
