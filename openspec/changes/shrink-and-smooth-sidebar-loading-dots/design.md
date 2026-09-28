<!-- refactor-layout-map sha256:e2eb7cc27deba8a1e0bfb8fa3e6771094a4ccb875d33cab20578289cd5198965 -->
源码路径迁移请按 `scripts/refactor/src_layout_map.tsv` 换算；本设计中的历史路径保留。

## Context

`SessionAttentionIndicator` renders four spans in a 16px slot. CSS owns both the 6.47-second linear orbit and 2.156-second radial breathing. Identical keyframes at 36% and 72% create a 776ms radial hold; 5.5px dots overlap at the 2.35px inner radius. See proposal.md for motivation.

## Goals / Non-Goals

Keep the existing CSS animation and component lifecycle. No session-state, row-layout, timing-duration, or unrelated composer changes are needed.

## Decisions

- Set each dot's width and height to 4.3px, with centered margins of -2.15px, following the user's refinement after the initial 2.75px version. Keep the 16px container and 5px outer radius, and use a 3.5px inner radius so the larger dots remain separate during contraction.
- Replace the shared 36%/72% contracted keyframes with a single 50% keyframe. Preserve eased breathing and linear rotation, with no finite stationary interval. JavaScript timers would add lifecycle complexity without addressing the CSS hold.
- Keep accent tokens and status accessibility labels. Reduced motion uses a gentle opacity-only pulse on the four-dot group, with no rotation or radial movement.

## Risks / Trade-offs

- Smaller dots need to remain distinct at normal zoom: check built styles in Chromium at desktop and 390px widths, in light/dark themes and Chinese/English fixtures.
- Browser checks establish frontend behavior; a running installed desktop still needs rebuilt embedded assets to receive this change.

## Validation

Measure the old radial hold, then sample the final animation through multiple cycles and former hold intervals. Check 4.3px diameter, four separate dots, stable row geometry, hover continuity, and reduced-motion behavior. Run the existing sidebar test, full web tests/build, strict OpenSpec validation, and diff checks.

## Older WebView2 compatibility follow-up

The production CSS was tested in Chromium 109.0.5412.0 and 145.0.7632.6. Both run all five original transform animations with normal motion preferences, but both stop every animation when `prefers-reduced-motion: reduce` matches. This reproduces a concrete static-indicator condition; the affected user's exact runtime and Windows settings remain unconfirmed.

Use `top`, `left`, `width`, and `height` for the orbit instead of `inset`. Explicitly define the rotation's starting transform and the dots' resting positions, so geometry does not depend on animation initialization. Retain the existing CSS keyframe approach; the tested old engine already supports it, so a JavaScript timer or user-agent branch is unnecessary.

In reduced-motion mode, fix all four dots at their 3.5px radius and animate only group opacity between 0.6 and 1 over 2.4 seconds. This retains running feedback without spatial movement or abrupt flashing. A browser regression script renders the production component against built CSS, checks both motion preferences, and accepts an older Chromium executable for repeatable compatibility checks. These tests do not substitute for an affected-machine WebView2 host check or claim whole-app compatibility with every older browser.
