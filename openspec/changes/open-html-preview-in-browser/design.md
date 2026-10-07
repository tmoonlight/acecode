## Context

See proposal.md for motivation. FilePreviewContent renders the source toolbar; PreviewDetailsPanel supplies file context; ChatView owns browser page creation and the unsaved-file guard. The existing Agent Browser accepts encoded local file URLs.

## Goals / Non-Goals

Reuse preview path resolution, native browser ownership, toolbar styling and unsaved-file handling. No system-browser launch, temporary HTML copy, HTTP server or implicit disk write is introduced.

## Decisions

- Resolve the file URL in a pure helper using previewAbsolutePath and normalizeAgentBrowserAddress. Restrict input to HTML/HTM and absolute resolved paths so a missing root cannot become a web search.
- Pass the existing browser-opening callback through PreviewDetailsPanel to FilePreviewContent. Extend it with an optional URL and navigate the newly created page using the existing bridge. This preserves session ownership instead of bypassing ChatView from a leaf component.
- Reuse the globe icon and ace-code-action-btn styling before the wrap action. Hide the action in runtimes without native browser support.
- Reuse save/discard/cancel before opening. The browser reads the saved disk file, never an unsaved draft. Guard session changes across asynchronous work and prevent duplicate opens while pending.

## Risks / Trade-offs

- Local file availability depends on the Desktop host filesystem. Browser navigation errors use the existing toast mechanism.
- Native rendering differs by platform. Browser fixtures verify bridge arguments and tab behavior; Windows packaged acceptance verifies the final release. macOS package CI remains a separate gate.
