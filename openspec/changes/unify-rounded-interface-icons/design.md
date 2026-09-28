<!-- refactor-layout-map sha256:e2eb7cc27deba8a1e0bfb8fa3e6771094a4ccb875d33cab20578289cd5198965 -->
源码路径迁移请按 `scripts/refactor/src_layout_map.tsv` 换算；本设计中的历史路径保留。

## Context

The current public family contains 86 SVGs on 16, 20, 24 and 48-unit grids; 78 contain literal paint colors. The generator covers only 63 files and disagrees with several committed outputs. Runtime CSS masks inherit color, but direct images and the fallback cannot reliably follow arbitrary theme colors. Functional inline SVGs and typographic substitutes add independent styles. Existing unrelated editor changes are present and must be preserved.

## Goals / Non-Goals

- Give every first-party functional icon one canonical shape and one color inheritance rule.
- Preserve aliases, static asset URLs, rotation, expanded-state distinctions, accessibility and control layout.
- Brand artwork, Seti/PPTX file-type artwork, native application/tray logos, data charts and terminal character rendering remain outside this frontend change.
- Do not distribute the extracted Claude font or paths. Author ACECode geometry using the measured style as reference.

## Decisions

1. Store authored SVG primitive arrays under `web/src/lib/icons/`, aggregated by `interfaceIcons.js`. Use a 20-unit square, approximately 2–3 units of outside padding, 1.5–2-unit rectangle radii, curved transitions on bent strokes, and round caps/joins. A canonical registry serves React and the standalone SVG exporter. This avoids another vendor dependency and prevents generated/runtime geometry drift.
2. Inline SVG inside the existing VsIcon wrapper renders known functional symbols using currentColor. The renderer computes the drawing stroke from the displayed size so 16/20/24 px yield 1/1.2/1.4 px strokes; intermediate sizes interpolate the same rule. The public SVG files use the 20-unit regular master and remain compatible with non-React mask consumers. Such statically scaled masks differ slightly from optical sizing; common fixed-size masks may use exported size variants when needed.
3. Keep existing public filenames and aliases. Filled panels retain a filled pane within the same rounded frame. Pin retains its current diagonal direction. Generic document/folder operation assets are redrawn, while actual file-type rendering and its data remain byte-preserved.
4. Add semantic definitions for inline controls and migrate local wrappers to VsIcon. Dynamic progress diagrams retain their value-driven geometry; typography, chart paths, brand illustrations and user-supplied SVG content are not mistaken for icons.
5. Enforce a paint whitelist (`none`, `currentColor`) and reproducible exports. No color literals, gradients, white/black cutouts, raster embeds, external icon fonts or literal-paint masks in the functional family. Control CSS remains responsible for status/accent colors.
6. Produce an inventory/gallery with every canonical glyph, native-size samples, enlarged geometry and light/dark contexts. Review the actual shared renderer as well as the export files.

## Risks / Trade-offs

- Thin small symbols can lose recognizability → simplify overlapping motifs, inspect every glyph at 16/20/24 px, and retain meaning rather than copying old outlines mechanically.
- Existing tests lock old vendor geometry → replace those constraints with semantic shape, color inheritance, coverage and deterministic regeneration checks; retain behavioral/accessibility assertions.
- Inline controls occur in dirty editor files → patch only the icon regions, preserve a pre-change snapshot, and inspect those diffs separately.
- Status colors formerly baked into artwork → keep or restore semantic color at the consuming UI layer, never in icon definitions.

## Migration Plan

Create the complete registry, replace generation and rendering, migrate scattered functional glyphs, update affected tests/documentation, run focused checks followed by the frontend suite/build and strict OpenSpec validation, then review the gallery and representative desktop/mobile layouts. Rollback is a scoped revert of this change; no data migration or backend dependency exists.
