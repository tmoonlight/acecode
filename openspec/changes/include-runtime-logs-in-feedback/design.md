## Context

The feedback package helper accepts one optional log file. Desktop/Web callers
select only `desktop-*.log`, even though the daemon process records the
session, provider, tool, and route activity that usually explains a failure.
The same limitation means browser-only deployments send no runtime log.

The TUI has a distinct `cwd/acecode.log` that remains useful and must not be
replaced. Package size must stay bounded, unavailable logs must remain
non-fatal, and existing consumers of `log_included`, `log_tail_bytes`, and
`included_files` must continue to work.

## Goals / Non-Goals

**Goals:**

- Package multiple log tails with an independent cap per source.
- Select the newest desktop and daemon rotated logs from the configured log
  directory.
- Keep Desktop/Web and TUI caller scopes explicit.
- Preserve aggregate metadata while adding per-log diagnostics.
- Make missing or unreadable log sources non-fatal and prevent zip entry-name
  collisions.

**Non-Goals:**

- Upload configuration, credentials, memory, workspace files, or unrelated
  session data.
- Change feedback upload transport, authentication, destination, or timeout.
- Concatenate complete rotated-log histories or increase the 512 KiB default
  cap for an individual source.

## Decisions

1. **Represent logs as a list of source descriptors.**

   `FeedbackPackageRequest` carries `FeedbackLogSource` values containing a
   path, optional zip entry name, and optional per-source byte cap. This keeps
   selection policy at the caller while centralizing tail reads, collision
   handling, and metadata in the package helper. Repeated single-log package
   calls were rejected because they would create multiple uploads and lose one
   coherent diagnostic snapshot.

2. **Use stable entry names for known runtimes.**

   The runtime collector selects the newest regular `desktop-*.log` and
   `daemon-*.log` by modification time and stores them as
   `logs/desktop.log.tail.txt` and `logs/daemon.log.tail.txt`. Stable names are
   easier for support tooling than date-bearing filenames. Arbitrary sources
   derive a sanitized name, and duplicate requested names receive a numeric
   suffix instead of overwriting an earlier entry.

3. **Keep caller scopes additive and explicit.**

   Desktop/Web feedback includes available desktop and daemon runtime logs.
   TUI `/feedback` keeps `cwd/acecode.log` and adds those same available
   runtime logs. A missing source is skipped; it does not remove another
   available source or fail an otherwise valid package.

4. **Extend metadata compatibly.**

   `log_included` remains true when any log is included and
   `log_tail_bytes` remains an aggregate, now summed across included tails.
   A new `logs[]` array records source path, final entry name, availability,
   and tail bytes for each requested source. `included_files` continues to
   list the actual archive entries.

## Risks / Trade-offs

- **Feedback packages can grow by one or two additional bounded tails.** →
  Keep the existing 512 KiB cap per source and expose the aggregate size.
- **A runtime log can rotate between discovery and reading.** → Treat the
  source as unavailable and continue packaging the remaining diagnostics.
- **Callers can request duplicate entry names.** → Allocate deterministic
  suffixed names before adding buffers to the zip.
- **Absolute source paths reveal local machine layout in diagnostics.** →
  This preserves the existing `log_path` behavior and is limited to an
  explicit user-triggered feedback package; no additional arbitrary paths are
  discovered or uploaded.
