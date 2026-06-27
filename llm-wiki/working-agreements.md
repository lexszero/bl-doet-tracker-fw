# Working Agreements

These rules are for AI agents and humans using the LLM wiki as a shared memory layer.

## Documentation discipline

- Keep [AI Agent Start Here.md](../AI%20Agent%20Start%20Here.md) as the stable entry point.
- Keep this wiki concise. Link to source files instead of copying their contents.
- Update the wiki when adding new subsystems, hardware assumptions, build workflows, or test workflows.
- Do not record guesses as facts. Mark uncertain items as known gaps.

## Code-change discipline

- Check `git status --short` before editing.
- Do not revert user changes unless explicitly asked.
- Prefer established Zephyr patterns in this repository for CMake, Kconfig, devicetree, drivers, tests, and docs.
- Keep changes scoped and make product documentation updates alongside product behavior changes.

## Collaboration discipline

- Ask the human collaborator before large downloads, large Docker image pulls/builds, remote infrastructure changes, or commands likely to consume significant disk/network/time.
- If an approach fails repeatedly, ask before downgrading the request, switching to a materially different strategy, or accepting a reduced outcome.
- Ask earlier when local-vs-remote workflow choices are ambiguous; the human collaborator can often unblock environment constraints directly.

## Verification discipline

- For firmware changes, prefer a relevant `west build` target when the board is known.
- For library or test changes, prefer focused Twister runs before broad test runs.
- For hardware behavior, prefer validation on an actual tracking device or an approved remote test bench.
- If a command cannot be run locally, document the reason in the final handoff.

## Wiki growth pattern

Start small and add pages only when a topic needs its own home. Good future pages may include:

- `hardware.md` for board, sensor, power, and pinout knowledge.
- `firmware-architecture.md` for runtime behavior and subsystem responsibilities.
- `build-and-flash.md` for validated local setup, board names, and flashing workflows.
- `testing.md` for useful test commands and expected coverage.
- `decisions.md` for dated architectural decisions.
