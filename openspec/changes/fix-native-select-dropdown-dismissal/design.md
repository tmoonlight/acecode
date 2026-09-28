<!-- refactor-layout-map sha256:e2eb7cc27deba8a1e0bfb8fa3e6771094a4ccb875d33cab20578289cd5198965 -->
源码路径迁移请按 `scripts/refactor/src_layout_map.tsv` 换算；本设计中的历史路径保留。

## Context

See proposal.md. Language and feedback use ordinary HTML selects with independent value handlers. Windows WebView2 152 reproduced transient native popup dismissal in a standalone page. Rendering the same controls with `appearance: base-select` kept all three test pickers open under physical mouse input. The exact internal runtime dismissal trigger remains unproven; changing host position notifications did not reliably resolve it.

## Goals / Non-Goals

Keep selection and form ownership in existing controls. Avoid per-page replacement components or synthetic mouse/selection events. Native host changes and runtime upgrades are outside this fix.

## Decisions

- Enable shared picker styles only for Windows Desktop and only when both base-select appearance and the picker selector are supported. Other browsers/platforms and older engines retain their existing native controls.
- Apply the rules to single-choice dropdowns, excluding multiple and multirow selects. Use existing theme tokens, bounded scrollable menus, and readable wrapping for long feedback labels.
- Preserve the real select DOM. The browser owns opening, focus, selection, accessibility, and `change` events; React continues owning the value.
- 菜单使用内容固有宽度，并以触发控件宽度作为最小值；最小和最大宽度都受视口限制。“每次询问”“最小化到托盘”“退出应用”等常规选项保持单行，超长标题或路径仅在菜单达到视口上限后换行。沿用浏览器的锚点定位和边缘回退，所有选项保持常规字重。
- Intercept propagation of Escape only while a browser-rendered select picker is open. Do not prevent its default action: the browser closes the picker, while the containing Modal stays open.

## Risks / Trade-offs

- Picker keyboard behavior follows the engine's customizable-select behavior. Validate mouse selection, keyboard selection, cancellation, and disabled options against actual controls.
- Unsupported older engines retain native rendering. This keeps them usable without introducing an untested JavaScript combobox fallback.
- Automated browser clicks did not consistently expose the native popup failure. Physical Windows mouse input is the regression acceptance check.

## Migration Plan

Ship through the normal Web asset build. Rollback consists of removing the shared opt-in and stylesheet; persisted configuration and select values require no migration.
