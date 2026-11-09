## Context

The tab panel owns breadcrumbs. Text, Markdown, HTML and Office renderers own their displayed content and available commands. Desktop Shell and Edge WebApp support opening files in the system file manager.

## Decisions

- Keep the horizontal path scroller and a fixed action slot as siblings. Render the active format's menu into the slot so commands use the current draft, zoom or slide directly.
- Keep the menu accessible for external files and sessions without a workspace. Preserve each format's shortcuts and existing zoom and slide limits.
- Resolve the file using its tab's working directory and share folder dispatch and error handling between the menus.
