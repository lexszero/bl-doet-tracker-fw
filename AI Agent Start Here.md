# AI Agent Start Here

This file is the first handoff point for AI agents working in this repository.

## LLM wiki

The LLM-maintained project wiki lives in:

- [llm-wiki/README.md](llm-wiki/README.md)

Read it before making code changes. Update it when you learn durable project knowledge, make architectural decisions, or add workflows that future agents should follow.

## Initial reading order

1. [README.md](README.md) for the human-facing project overview and build commands.
2. [llm-wiki/README.md](llm-wiki/README.md) for the AI-facing wiki index.
3. [llm-wiki/documentation-boundaries.md](llm-wiki/documentation-boundaries.md) for public/private documentation rules.
4. [llm-wiki/current-handoff.md](llm-wiki/current-handoff.md) for the latest public-safe cross-chat state.
5. [llm-wiki/project-context.md](llm-wiki/project-context.md) for ownership and product context.
6. [llm-wiki/github-issues.md](llm-wiki/github-issues.md) for Lex's current public issue summaries.
7. [llm-wiki/branch-inventory.md](llm-wiki/branch-inventory.md) for remote branch context.
8. [llm-wiki/project-map.md](llm-wiki/project-map.md) for the current repository layout.
9. [llm-wiki/development-process.md](llm-wiki/development-process.md) for device access and validation expectations.
10. [llm-wiki/working-agreements.md](llm-wiki/working-agreements.md) for collaboration and documentation rules.
11. The source, config, board, driver, or test files relevant to the current task.

If available and relevant, also read ignored private notes:

- `llm-wiki/private/dev-group/current-bench-access.md` for shared bench/backend access and runbooks.
- `llm-wiki/private/personal-agent/README.md` for this local agent's machine-specific state.

Do not copy private-note content into tracked files.

## Before changing files

- Run `git status --short` and preserve user changes.
- Prefer the existing Zephyr module/application patterns already present in the repository.
- Keep changes scoped to the task.
- Add or update LLM wiki notes when the change creates knowledge future agents need.
- Keep public docs, development-group private docs, and personal-agent private docs separated.

## Current project posture

This repository is for firmware related to tracking where Burn vehicles are during the Bolderland Burn. It currently resembles a Zephyr out-of-tree application/module scaffold, and some files still contain upstream example-application wording. Treat that inherited wording as cleanup context, not authoritative product documentation.
