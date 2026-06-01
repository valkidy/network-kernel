---
name: token-aware-planning
description: Use when Codex creates or executes implementation plans and must control token, cost, file-reading, search, edit, and validation scope; especially for smoke-test-first workflows and tasks where broad repository exploration or full builds should be avoided.
---

# Token-Aware Planning

## When to Use This Skill

Use this skill when producing or executing an implementation plan, choosing validation scope, or reporting completion for tasks where token or execution cost matters.

## Goal

Treat token and cost usage as part of the task constraints. Complete work with controlled exploration, limited file reading, minimal unnecessary output, and staged validation.

## Core Principles

- Optimize for correctness, minimal scope, low token usage, fast feedback, and clear validation boundaries.
- Prefer smoke-test-first workflows.
- Do not assume that more context, more file reads, broader searches, or larger tests are always better.
- Stop and explain before expanding scope beyond the agreed budgets.

## Required Plan Sections

Every token-aware plan must include these sections.

### Goal

State the single task outcome in one or two sentences.

### In Scope

List the work that will be performed.

### Out of Scope

List what will not be changed, including full builds, full tests, docs, platforms, or subsystems that are excluded.

### File Budget

State the maximum number of files to read before proposing changes. Prefer directly related source, build, test, or skill files. If the budget must expand, stop and explain why.

### Search Budget

State allowed searches and avoided searches. Prefer targeted searches under relevant directories. Avoid full-repo search unless the exact symbol or path justifies it.

### Edit Budget

State the maximum number of files to modify. Avoid unrelated formatting, opportunistic refactors, or cleanup outside the task boundary.

### Validation Budget

Use staged validation:

- Level 0: static review only, such as `git diff --stat` or targeted diff review.
- Level 1: smallest relevant smoke build or test.
- Level 2: local regression after smoke passes.
- Level 3: final regression only when explicitly requested.

### Expansion Rule

If more files, broader search, more edits, or stronger validation are required, stop and explain which limit must expand and why before continuing.

### Token / Cost Tracking

At completion, report exact token usage if available. If exact usage is unavailable, say so and use proxy metrics instead.

## Completion Report Template

Use this concise report shape:

```text
Result:
- <completed work>

Files changed:
- <path>

Validation:
- Level 0: pass/fail/not run - <command or reason>
- Level 1: pass/fail/not run - <command or reason>
- Level 2: pass/fail/not run - <command or reason>

Token / cost awareness:
- Exact token usage: available/unavailable
- Files read: N
- Files modified: N
- Searches performed: N
- Build/test commands executed: N
- Broad repo search used: yes/no
- Full build/test used: yes/no
- Largest cost driver: <file reading/search/build logs/repeated failures/long output>

Deferred follow-ups:
- <item or none>
```

## Default Limits

- Read at most 3-5 files before editing.
- Modify at most 1-3 files.
- Run only the smallest relevant smoke test first.
- Avoid full-repo search.
- Avoid full build/test.
- Report token and cost proxy metrics at the end.
