# LLM Wiki

This wiki is the AI-agent knowledge base for this repository. It should stay small, factual, and easy to extend.

## Purpose

- Capture durable context that helps future agents work safely.
- Point to source-of-truth files instead of duplicating large sections of code or docs.
- Record project conventions, workflows, decisions, and known gaps as they become clear.

## Index

- [Project context](project-context.md): product purpose, ownership, and current framing.
- [Documentation boundaries](documentation-boundaries.md): what belongs in public GitHub docs, shared private notes, and personal-agent notes.
- [Current handoff](current-handoff.md): latest cross-chat state and next steps.
- [GitHub issues snapshot](github-issues.md): current public issue summaries from Lex's referenced tracker repository.
- [Branch inventory](branch-inventory.md): remote branch snapshot and tracker-branch notes.
- [Project map](project-map.md): repository structure and important entry points.
- [Build environment](build-environment.md): local Docker build workflow and Zephyr version direction.
- [Development process](development-process.md): device access, remote validation, and development expectations.
- [Storage and logging](storage-and-logging.md): current flash layout, logging options, and open SD-card questions.
- [LoRa live logging](lora-live-logging.md): ChirpStack receiver-side position logging and live map workflow.
- [Working agreements](working-agreements.md): how AI agents should change code and maintain this wiki.

## How to update this wiki

- Add facts only after verifying them in the repository or through the user.
- Prefer short sections with links to files.
- Date decisions when they are time-sensitive or likely to evolve.
- Move stale assumptions into a "Known gaps" section instead of leaving them as facts.
- Keep tracked wiki pages public-safe. Put operational bench/backend details in ignored development-group notes and local agent state in ignored personal-agent notes.

## Known gaps

- Product-specific firmware behavior is only lightly documented so far; the initial assignment is summarized in [Project context](project-context.md) and [GitHub issues snapshot](github-issues.md).
- Detailed board revisions and Zephyr targets for the Dragino TrackerD-LS and TTGO T-Beam units need confirmation.
- The tracker proof-of-concept currently lives on `dev/tracker`; see [Branch inventory](branch-inventory.md).
