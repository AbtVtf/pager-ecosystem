# Contributing to PagerOS

Thanks for your interest in PagerOS. The project is in **pre-alpha**, built primarily by an agent team coordinated through [Paperclip](https://github.com/paperclipai), with direct-push-to-main gated by conformance tests. Outside contributions are welcome and follow the same rules the agents do.

Before you start, please read:

- [`SPEC.md`](./SPEC.md) — the technical specification (single source of truth)
- [`TASKS.md`](./TASKS.md) — the full task list with IDs, deps, and acceptance criteria
- [`CLAUDE.md`](./CLAUDE.md) — agent operating manual; humans should follow the same workflow

## How to contribute

### 1. Pick a task

- Find or open an issue. If a `TASKS.md` ID applies (e.g. `PROTO-002`, `PY-004`), reference it.
- Cross-cutting work (touching `SPEC.md`, `TASKS.md`, `CLAUDE.md`, `README.md`, `.gitignore`) requires CEO sign-off — open an issue first.

### 2. Stay in your subsystem

Each subsystem lives in its own directory. Touch only that directory plus shared docs that explicitly cover it:

| Prefix | Directory                                          |
| ------ | -------------------------------------------------- |
| PROTO  | `protocol/`                                        |
| FW     | `firmware/`                                        |
| PY     | `sdk/python/`                                      |
| JS     | `sdk/js/`                                          |
| SIM    | `simulator/`                                       |
| CLI    | `cli/`                                             |
| LORA   | `firmware/` (client) + `exit-node/` (node); spec in `protocol/` |
| EXIT   | `exit-node/`                                       |
| PUSH   | `push-relay/`                                      |
| MKT    | `marketplace/`                                     |
| SEC    | crosscuts; mostly `docs/spec/`                     |
| DOCS   | `docs/`, `examples/`                               |

QA and CEO are the only roles permitted to edit across subsystems.

### 3. Branch, commit, push

The default workflow is direct-push-to-main (with conformance-test gates). Outside contributors should fork and open a pull request against `main`. Either way:

- One coherent commit (or a short series) per task.
- **Reference the task ID** in the commit subject: `[PROTO-001] Define widget tag registry`.
- Rebase, don't merge: on push rejection, `git pull --rebase origin main`, resolve conflicts only inside your subsystem, push again.
- **Never** `git push --force` or `git reset --hard origin/main`. Other work is in flight.
- **Never** bypass commit hooks with `--no-verify`.

## Code style

Each subsystem follows the conventions of its language toolchain. Run formatters and linters before committing — the commit hooks will check them.

| Language          | Formatter        | Linter / type check         | Notes                                                        |
| ----------------- | ---------------- | --------------------------- | ------------------------------------------------------------ |
| Python (`sdk/python/`, `push-relay/`, `marketplace/`) | `ruff format`    | `ruff check`, `mypy --strict` | Target Python 3.10+. Type-annotate public APIs.              |
| JS/TS (`sdk/js/`, `marketplace/web/`)                  | `prettier`       | `eslint`, `tsc --noEmit`    | TypeScript with `"strict": true`.                            |
| Go (`cli/`, `exit-node/`)                              | `gofmt -s`       | `go vet ./...`, `staticcheck` | Use the standard layout; no third-party logging frameworks. |
| C/C++ (`firmware/`)                                    | `clang-format` (LLVM style with 4-space indent) | `cppcheck --enable=warning` | ESP-IDF target. No exceptions, no RTTI.                      |
| Markdown (`docs/`, root `*.md`)                        | `prettier`       | `markdownlint`              | Wrap at 100 cols, fenced code blocks with language tag.      |

Additional house rules:

- **Filenames:** `kebab-case` for docs and configs, `snake_case` for Python modules, `camelCase` for JS/TS, `lower_snake.c` for firmware sources.
- **Public symbols** must have a docstring/comment that says *what* and *why*, not *how*.
- **No dead code.** Remove disabled blocks rather than commenting them out.
- **No secret material** committed — keys, tokens, device serials, `.env` files. CI will fail the build.

## Pull request process

1. **Open an issue first** (or claim an existing one) for anything beyond a typo fix. PRs without a tracked task may be closed.
2. **One PR per task ID.** Don't bundle unrelated changes.
3. **Title** mirrors the commit subject: `[<TASK-ID>] short description`.
4. **Description** must include:
   - What changed and why (2–5 sentences).
   - The acceptance criteria from `TASKS.md` (or the issue) and how each is satisfied.
   - Manual test notes if the change touches user-visible behavior.
   - Links to any related issues, spec sections, or design docs.
5. **CI must be green.** Builds, lints, unit tests, and the protocol conformance suite (once `PROTO-005` lands) all gate merge.
6. **Review:** at least one approval from a maintainer in the affected subsystem. QA can request changes on any PR.
7. **Merging:** maintainers rebase-and-push to `main`. The PR author should not force-push after review starts; use additional commits and let the maintainer squash on land.
8. **Reverts:** QA may `git revert <sha>` on red `main`. Don't `reset --hard`.

## Sign-off (DCO)

Every commit must be signed off under the [Developer Certificate of Origin](https://developercertificate.org/). By signing off, you certify that you wrote the patch yourself, or otherwise have the right to submit it under the project license.

Add this line to the end of each commit message:

```
Signed-off-by: Jane Doe <jane@example.com>
```

The easiest way is to pass `-s` to `git commit`:

```
git commit -s -m "[PROTO-001] Define widget tag registry"
```

Commits without `Signed-off-by` will be rejected by CI. Use your real name and a real email address — pseudonyms are not accepted.

Agents committing on behalf of the project add a co-author trailer in addition to the sign-off:

```
Co-Authored-By: Paperclip <noreply@paperclip.ing>
```

## Testing requirements

A change is not done until its tests pass. **Do not mark an issue done with failing tests.**

### Required test coverage

| Change type                          | Required tests                                                                 |
| ------------------------------------ | ------------------------------------------------------------------------------ |
| Protocol / wire format               | Cross-language test vectors in `protocol/test-vectors/` (PROTO-003) + conformance suite. |
| SDK feature (Python or JS)           | Unit tests in the SDK's `tests/` directory; both SDKs must match behaviour.    |
| Simulator / CLI / push-relay / marketplace / exit-node | Unit tests in the subsystem's `tests/` directory.                              |
| Firmware                             | On-host unit tests where feasible (`firmware/test/`); device smoke test documented in the PR if hardware is required. |
| Docs / examples                      | Examples must build with `pagerctl init <template>` and run end-to-end in the simulator. |

### Running tests

| Subsystem        | Command                                                       |
| ---------------- | ------------------------------------------------------------- |
| Python SDK       | `cd sdk/python && pytest`                                     |
| JS SDK           | `cd sdk/js && pnpm test`                                      |
| Simulator        | `cd simulator && pnpm test`                                   |
| CLI              | `cd cli && go test ./...`                                     |
| Exit node        | `cd exit-node && go test ./...`                               |
| Push relay       | `cd push-relay && pytest`                                     |
| Marketplace      | `cd marketplace && pytest` (api) / `pnpm test` (web)          |
| Firmware (host)  | `cd firmware && idf.py test` (after `source ~/esp/esp-idf/export.sh`) |
| Conformance      | `pagerctl conform` (once `PROTO-005` and `CLI-003` land)      |

If you add a dependency, also add the version pin to the subsystem's lockfile and explain the choice in the PR description.

### When you can't run the acceptance check

If a task's `Accepts` clause requires hardware or infrastructure you don't have (e.g. a real T-LoRa Pager, a LoRa neighbour, an SMTP server), state that explicitly in the PR description. A maintainer or QA agent will run the missing check before merge.

## Reporting bugs and security issues

- **Bugs:** open an issue with reproduction steps, expected vs actual behaviour, firmware/SDK version, and relevant logs.
- **Security:** *do not* open a public issue. Email security@pageros.org (placeholder until the project domain is live) with a description and reproduction steps. We aim to acknowledge within 7 days.

## License

The project license is **TBD** and will be finalised before the first tagged release. By contributing you agree to license your contribution under whatever license the project adopts, consistent with your DCO sign-off. Existing source files carry an `MIT` placeholder (`sdk/python/pyproject.toml`); the final choice will be applied uniformly across the tree.

## Code of conduct

Be kind, be specific, be patient with new contributors. Harassment, personal attacks, or sustained disruption are grounds for removal from the project. Report incidents to conduct@pageros.org.

---

Questions that don't fit an issue? Open a discussion in the [Paperclip](https://github.com/paperclipai) workspace or ping CEO via an issue assigned to them.
