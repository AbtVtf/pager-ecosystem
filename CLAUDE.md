# PagerOS — Agent Operating Manual

You are a specialist agent in a multi-agent team building PagerOS. This file applies to every agent.

## Read these first
- `SPEC.md` — technical specification v0.2 (single source of truth)
- `TASKS.md` — full task list with IDs and dependencies

## Your assignment
Paperclip has assigned you an issue. The issue's ID prefix (PROTO / FW / PY / JS / SIM / CLI / LORA / EXIT / PUSH / MKT / SEC / DOCS) maps to your subsystem. The issue description references a task in `TASKS.md` by its full ID (e.g., `PROTO-001`).

**Only work on the assigned task.** Do not start adjacent tasks even if they look easy. If you finish early, mark the issue done and the next assignment will come.

## Workflow on every run

1. **Sync.** `git pull --rebase origin main` — get latest before doing anything.
2. **Mark in_progress** in Paperclip immediately on starting.
3. **Verify deps.** If the task's `Deps` field lists IDs that aren't done yet, do not implement — leave a comment on the issue and mark it `pending` again.
4. **Do the work.** Implement to the acceptance criteria in `TASKS.md`. Stay inside your subsystem's directory in the repo (`firmware/`, `sdk/python/`, etc.).
5. **Test.** Run whatever tests exist. Add tests if the task implies them. Do not mark done with failing tests.
6. **Commit + push.** One coherent commit (or a short series). Reference the task ID in the message: `[PROTO-001] Define widget tag registry`. Push to `origin/main`. On push rejection: `git pull --rebase`, resolve any conflicts (only inside your subsystem), push again.
7. **Mark done** in Paperclip with a comment summarizing what landed.

## Rules

- **Stay in your lane.** Do not edit files outside your subsystem. The QA agent and CEO are the only exceptions.
- **Do not modify** `SPEC.md`, `TASKS.md`, `CLAUDE.md`, `README.md`, or `.gitignore` without explicit authorization from CEO (assign an issue back to CEO if you think one of these needs to change).
- **Never** `git push --force` or `git reset --hard origin/main`. Other agents have work in flight.
- **Never** `--no-verify` a commit hook.
- **Firmware agents:** always `source ~/esp/esp-idf/export.sh` before `idf.py` commands. Device is on `/dev/ttyACM0`.
- **All agents:** if you get stuck or a dep is missing, leave a comment and re-queue (do not invent dependencies or build outside spec).

## Subsystem ↔ directory map

| ID prefix | Directory |
|---|---|
| PROTO | `protocol/` |
| FW | `firmware/` |
| PY | `sdk/python/` |
| JS | `sdk/js/` |
| SIM | `simulator/` |
| CLI | `cli/` |
| LORA | shared between `firmware/` (client side) and `exit-node/` (node side); spec in `protocol/` |
| EXIT | `exit-node/` |
| PUSH | `push-relay/` |
| MKT | `marketplace/` |
| SEC | crosscuts; mostly docs in `docs/spec/` |
| DOCS | `docs/`, `examples/` |

## Coordination

- **CEO** owns cross-cutting decisions, can re-prioritize, can spawn new issues.
- **QA** runs the conformance test suite (once PROTO-005 lands) and is allowed to revert breaking commits with `git revert <sha>` (never reset).
- Cross-subsystem questions → open an issue assigned to CEO.

## What "done" means

A task is done when its **Accepts** clause in `TASKS.md` is observably true. If you can't run the acceptance check (e.g., it needs hardware you don't have), state that explicitly in the close comment and request CEO confirm.
