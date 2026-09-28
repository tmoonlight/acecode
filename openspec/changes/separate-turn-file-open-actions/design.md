<!-- refactor-layout-map sha256:e2eb7cc27deba8a1e0bfb8fa3e6771094a4ccb875d33cab20578289cd5198965 -->
源码路径迁移请按 `scripts/refactor/src_layout_map.tsv` 换算；本设计中的历史路径保留。

## Context

TurnFileList currently renders each item as one button with a decorative “打开” span. Its onOpenFile prop receives openSessionChangePreview at both ChatView call sites. ChatView already owns openFilePreview, which resolves workspace paths and opens file-content tabs.

## Goals / Non-Goals

**Goals:** Reuse the existing preview owners and retain turn UUIDs exclusively on the change action.

**Non-Goals:** New viewer, external editor launch, backend protocol changes, or sidebar changes.

## Decisions

- Preserve the original row's spacing, background, border and right-side “打开”. A native file button covers the row, with the decorative filename/counts/open label allowing pointer events through. A separate native “查看变更” button sits above that hit target beside the filename. This avoids nested controls and prevents diff clicks from invoking file preview.
- Name callbacks onOpenChanges(file, turnUserMessageId) and onOpenFile(file). Wire both ChatView placements to existing owners. Never pass the turn UUID as openFilePreview's optional line argument.
- At rest, retain the original counts and layout. Row hover or keyboard focus replaces counts with muted “查看变更” plus a small up-right SVG arrow. Underline only the hovered change label. Keyboard focus remains visible; non-hover input exposes the secondary action for touch access.
- Add an English source override and regenerate the catalog through the existing toolchain.

## Risks / Trade-offs

- The hover action consumes path space → preserve truncation and inspect long paths at narrow widths; retain the full path tooltip on the main file target.
- Historical files can be moved or deleted → use existing file-preview error behavior; recorded diffs remain available independently.

## Validation

Update the existing turn-identity regression assertion for the renamed callback, run focused and full web tests plus build and strict OpenSpec validation, and exercise the real component in a browser for independent mouse/keyboard actions, folding, themes and localization.

### Results

- Focused turn-file tests, `pnpm test`, `pnpm i18n:catalog`, `pnpm build`, `openspec validate separate-turn-file-open-actions --strict`, and `git diff --check` passed.
- Browser checks rendered the production TurnFileList and CSS with fixture data and the existing preview-tab reducers. Filename, icon, blank space and “打开” each invoked only file preview; the change action invoked only the turn-scoped preview and preserved the turn UUID.
- Verified row hover without underlining, change-action hover with underlining, Enter/Tab/Space activation with visible focus, expand/collapse, and an absolute Chinese path outside the workspace.
- Inspected Chinese/light at desktop width and English/dark at 390px without horizontal overflow. No browser page errors were reported.
- The design detector reported no findings in this component or changed CSS section; its global stylesheet findings were in pre-existing unrelated sections.
- Browser validation used a component fixture; a live daemon/desktop integration was not run. The CLI's iPhone preset retained `(hover: none) = false`, so the CSS touch fallback was reviewed in source but not verified under actual no-hover emulation.
