## 1. Implementation

- [x] 1.1 Add scoped Ctrl+Enter activation through the primary button with overlay, editor, IME, repeat, sending and lifecycle guards.
- [x] 1.2 Add compact keycaps and accessible shortcut metadata to the primary button, preserving narrow layout.

## 2. Verification

- [x] 2.1 Verify real ChatView keyboard confirmation, draft preservation, event isolation and responsive light/dark rendering.
- [x] 2.2 Run pnpm test, pnpm build, OpenSpec strict validation, design detector and scoped diff checks.

## Verification evidence

- `node scripts/test-next-step-shortcut.mjs` passed using Chromium and production ChatView/Slate: primary reply once, draft preservation, exact modifiers, IME and composition settling, repeated key suppression, dialog/menu/editor/terminal/side-chat isolation, disabled sending, dismissal cleanup, secondary pointer action, and StrictMode cleanup. Screenshots at `C:/Users/shao/AppData/Local/Temp/ace-next-step-JbYbMV/light.png` and `dark.png` were visually inspected; narrow long labels preserve both keycaps without overflow.
- `pnpm test` and `pnpm build` exited 0 after replacing unsupported slash-opacity border utilities with `border-current`. Build retained the existing large-chunk advisory; regex compatibility passed.
- `openspec validate confirm-next-step-shortcut --strict`, scoped `git diff --check`, and the Impeccable detector passed (no detector findings).
- This changes the Web source and generated local Web build. The installed desktop application was not rebuilt or relaunched.
