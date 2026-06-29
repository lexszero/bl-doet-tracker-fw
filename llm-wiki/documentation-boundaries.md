# Documentation Boundaries

Last updated: 2026-06-29.

This repository now uses three documentation visibility levels.

## Public / GitHub

Location: tracked repository files, including `README.md`, source comments, and tracked files under `llm-wiki/`.

Allowed content:

- build instructions that do not include private hosts, credentials, or device IDs;
- firmware architecture, payload formats, flash layout, diagnostic formats, and test methodology;
- public issue summaries and public project context;
- sanitized hardware observations and validation results.

Do not include:

- hostnames, ports, usernames, private key paths, passwords, tokens, internal IPs, tunnel commands, exact ChirpStack device IDs, exact backend URLs, or live bench file paths;
- personal local paths, local PIDs, local browser state, temporary tunnel logs, or one-agent-only continuity notes.

## Private / Development Group

Location: ignored files under `llm-wiki/private/dev-group/`.

Audience: people actively developing or operating the tracker system, currently Christian, Lex, Mohsen, and future trusted maintainers.

Appropriate content:

- shared bench access/runbook details;
- ChirpStack and LoRaWAN operational notes;
- provisioned tracker identity references;
- remote helper commands and deployment notes;
- private validation logs or exact backend paths needed by the development group.

These files are intentionally ignored by git. Share them through the group's private channel, not through public commits.

## Private / Personal Agent

Location: ignored files under `llm-wiki/private/personal-agent/`.

Audience: Christian and his local Codex/agent environment.

Appropriate content:

- local SSH alias and private key path notes;
- local tunnel scripts, PIDs, and logs;
- `.codex-local` paths, local map server process notes, and browser state;
- one-agent continuity notes that are not useful to the wider development group.

Personal-agent notes may reference development-group notes, but development-group notes should not require Christian's local machine paths to be useful.
