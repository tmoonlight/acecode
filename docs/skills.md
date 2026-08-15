# Skills

Skills are user-authored instruction documents that extend acecode with domain workflows (planning, debugging, deploying, etc.). Each skill is a Markdown file with YAML frontmatter that acecode discovers at startup and exposes to the LLM via **progressive disclosure** and to you via a dedicated slash command.

## Directory layout

Skills live under built-in roots `~/.acecode/skills/` and compatible `~/.agent/skills/`. The recommended layout is:

```
~/.acecode/skills/ or ~/.agent/skills/
  <category>/
    <skill-name>/
      SKILL.md            # required — frontmatter + body
      references/         # optional supporting markdown
      templates/          # optional templates
      scripts/            # optional executable helpers
      assets/             # optional binary assets
```

`<category>` is a free-form folder name (for example `engineering`, `writing`, `ops`). Skills at the top level (without a category folder) get category `"default"`. Extra root directories can be added via `config.skills.external_dirs`.

## Default seeded resources

At startup, ACECode compares the packaged `assets/seed/seed.version` revision with
`~/.acecode/seed.version` before the first Skill, expert, and hook registry scans. The revision uses
`YYYY-MM-DD.N`, where `N` is a numeric revision for that date. A missing, invalid,
or older user marker triggers an offline reconciliation of bundled Skills into
`~/.acecode/skills/`, bundled experts into `~/.acecode/experts/`, and managed hook
packages into `~/.acecode/hooks/`. An equal marker is normally a no-op; ACECode still
self-heals a missing managed hook directory or upgrades a recognized previous official
hook definition. Unknown and user-modified definitions remain preserved. A newer user
marker prevents an older installation from downgrading the bundle.

The default Skill bundle contains:

- `find-skills`
- `skill-installer`
- `skill-creator`
- `expert-manager`
- `native-mcp`
- `mcporter`
- `acecode-tui-usage`
- `acecode-desktop-usage`
- `vision-image-reader`

The expert bundle contains the OPC one-person-company team, its lead, and eight
stage experts under the `opc-*` package IDs.

The managed hook bundle contains `agent-reporting`, which connects the generic
ACECode lifecycle events to Herdr when ACECode is launched inside a Herdr pane.
It is a guarded no-op elsewhere. Seed reconciliation does not rewrite
`~/.acecode/hooks.json`, `~/.codex/hooks.json`, or project hook files.

Missing resources are installed. A previously seeded Skill, expert, or hook is updated
only when its complete installed directory still matches the ACECode-owned hash recorded in
`~/.acecode/.seed_skills_state.json`. Unknown directories and user-modified seeded
resources are preserved. ACECode atomically records the detailed reconciliation state
before advancing `~/.acecode/seed.version`, so an interrupted or failed run can be
retried on the next startup.

Managed seed hooks receive automatic trust only while their parsed JSON definition
matches the official fingerprint built into ACECode. Modified or malformed seed hook
files remain on disk but are not executed as managed hooks.

When the packaged seed bundle changes, update `assets/seed/seed.version` and keep
the same revision in `assets/seed/MANIFEST.json`.

## SKILL.md format

```markdown
---
name: plan
description: Author a short engineering plan before implementation.
category: engineering           # optional, inferred from folder when omitted
platforms: [windows, macos, linux]  # optional, defaults to all
tags: [planning, workflow]      # optional
---

# Plan

... body instructions the LLM will read when the skill is activated ...
```

**Required**: `name`, `description`. Everything else is optional. `name` is normalized to kebab-case for the slash command key (so `name: My Plan` → `/my-plan`).

## How skills activate

Two activation paths, both routed through the agent loop:

1. **User invocation** — Type `/<skill-name> [optional argument]`. acecode loads the full body, appends any supporting file listing and the optional argument, and submits the result as a user message. The LLM then follows the skill's instructions.
2. **LLM discovery** — The LLM sees a `# Skills` hint in the system prompt whenever at least one skill is installed, plus two tools:
  - `skills_list` — returns `[{name, description, category}, …]` plus lightweight discovery metadata such as `reason`, `fallback_applied`, and `available_categories`. Invalid or unknown `category` filters are ignored and fall back to the unfiltered list.
   - `skill_view` — returns the full SKILL.md body (tier-2) or a supporting file (tier-3). `{name, file_path?}`.

The LLM is expected to call `skills_list` when a task looks skill-shaped, then `skill_view` to load the chosen skill before acting.

## Configuration

`~/.acecode/config.json`:

```json
{
  "skills": {
    "disabled": ["plan"],
    "external_dirs": ["~/work/team-skills"]
  }
}
```

- `disabled` — skill names to hide even if the files are on disk.
- `external_dirs` — extra scan roots. `~` and `${ENV}` are expanded.

Both fields default to empty and the whole block may be omitted.

## Runtime commands

- `/skills` — open the full-screen capability center on the Skills tab.
- `/skills list` — show installed skills grouped by category in the transcript.
- `/skills reload` — rescan disk and re-register slash commands. Use after editing a SKILL.md.
- `/skills help` — print this summary inside the TUI.
- `/<skill-name>` — activate the skill directly.

## Safety

- `skill_view` rejects `..` in `file_path` and verifies the resolved path stays inside the skill directory before reading.
- Files larger than 2 MB are rejected to keep activation payloads bounded.
- Supporting-file listings are capped at the first match per category folder.

## Authoring tips

- Keep the body focused — the whole thing ships to the LLM on every activation.
- If a skill needs reference material, put it in `references/` and instruct the LLM to call `skill_view` to fetch it, rather than pasting it inline.
- Use `platforms` when a skill is OS-specific. Leave it off for portable skills.
- Write instructions in the imperative, second person. The LLM is the audience.

See `examples/skills/plan/SKILL.md` for a starting template.
