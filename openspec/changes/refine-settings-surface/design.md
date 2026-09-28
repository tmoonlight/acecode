<!-- refactor-layout-map sha256:e2eb7cc27deba8a1e0bfb8fa3e6771094a4ccb875d33cab20578289cd5198965 -->
源码路径迁移请按 `scripts/refactor/src_layout_map.tsv` 换算；本设计中的历史路径保留。

## Context

See proposal.md for the visual defects. SettingsPage currently wraps both columns below a 40px title bar. Scoped CSS already flattens legacy cards but adds row borders without removing the old group separators. The shared stylesheet contains unrelated local edits.

## Goals / Non-Goals

Keep the change within the Settings shell and its existing style adapter. Do not alter configuration logic, modal dimensions, font sizing, or application-wide colors.

## Decisions

- Render navigation beside a full-height main column. Place existing window actions at the upper right of that column and reserve space above the scroll area; retain a visually hidden dialog name.
- Derive Settings surface tokens from the existing surface and foreground tokens. Use these for neutral navigation states and subtle dividers, and use the content surface for standard form fields. Avoid changing global theme tokens or hardcoding colors in components.
- Turn the legacy my-5 group rule into whitespace inside Settings. Retain row separators and inner form boundaries rather than restoring card frames or suppressing all borders.
- Preserve the current panel size and narrow-screen navigation behavior. Check the right column and actions at scaled desktop sizes, narrow widths, dark mode, and large fonts.
- 字重按语义层级维护：主标题保留当前标题字重，一级分组标题保留半粗体；其余字段、选项、卡片、导航和统计值统一为 400，使用 `text-fg` 与 `text-fg-mute` 区分主次。直接修正设置组件及其专属 CSS，不使用全局强制覆盖。设置打开的模型表单和主题导入中的下级标题遵守同一规则，独立弹窗主标题可保留加粗。
- 保留字号、字体栈、颜色体系、布局和交互；同步仓库前端样式规范，避免后续新增设置恢复旧的加粗字段样式。浏览器验证使用模拟配置，覆盖明暗主题、中英文、窄屏和设置子弹窗。

## Risks / Trade-offs

- Shared CSS may affect embedded settings sections: inspect general, appearance, config, and representative list/form sections.
- Overlay actions could cover scrolled content: reserve their space in the right column instead of floating them over its scrollport.
- Live configuration must remain unchanged during visual checks: use a temporary preview that renders the real component with mocked API responses.
