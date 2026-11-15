# securaCV — Claude Code notes

@AGENTS.md

That import is the whole brief and all a session carries (a tool that does not
expand `@` imports reads `AGENTS.md` first). Everything else loads by need: a tree's
brief when you touch a file in it (its one-line `CLAUDE.md` imports the
`AGENTS.md` beside it, since a root `CLAUDE.md` turns off Claude Code's own
`AGENTS.md` discovery), a rule in `.claude/rules/` when you touch a file its
`paths:` match. Open one directly when the task is about that area.

| Working on | Rules load from |
|---|---|
| `src/`, the kernel | [`src/AGENTS.md`](src/AGENTS.md) — the invariants in code, the backend audit, deps, `cargo` checks |
| `firmware/` | [`firmware/AGENTS.md`](firmware/AGENTS.md) — which copy is the source, host tests, generated truth |
| the WAP: Beacon, Chirp, mesh | [`firmware/projects/canary-wap/AGENTS.md`](firmware/projects/canary-wap/AGENTS.md) |
| the displays and the emulator dist | [`firmware/projects/canary-display/AGENTS.md`](firmware/projects/canary-display/AGENTS.md) |
| the Lab, `canary-local/` | [`canary-local/AGENTS.md`](canary-local/AGENTS.md) — the page tests, the two dist gates, the generators |
| enclosures and device manifests | [`docs/hardware/enclosure/AGENTS.md`](docs/hardware/enclosure/AGENTS.md) — the CAD chain, previews, knobs |
| workflows and releases | [`.github/AGENTS.md`](.github/AGENTS.md) — the buttons, the lessons, the dist rebuild |
| any input of a generated file | [`.claude/rules/generated.md`](.claude/rules/generated.md), path-scoped; the account is [`docs/GENERATED_FILES.md`](docs/GENERATED_FILES.md) |

## Reading

- Twelve hand-edited files exceed 150 KB (`canary_wap.ino` is 685 KB). Search
  first, then read a line range. A hook refuses a whole read of a file over
  120 KB and prints its outline instead (`scripts/hooks/read_guard.py`).
  `.claude/settings.json` hides compiled and binary artifacts from the file
  tools; `.ignore` keeps ripgrep out of the dumps (`rg --no-ignore -g <file>`
  when you mean one).
- The lesson ledgers (`.github/RELEASE_LESSONS.md`, 93 entries;
  `firmware/LESSONS_LEARNED.md`, 169) are grepped for the target you are on,
  never read whole. Append an entry when you pay for a new lesson.
- A long read belongs in a subagent: it returns the paragraph, not the file.

## The budget

`python3 scripts/agent_context_budget.py --check` (in lint.yml) caps this file
with its import, and every tree brief and rule. A rule for one tree goes in
that tree's brief; a story about why goes in `docs/`; the root stays small.
