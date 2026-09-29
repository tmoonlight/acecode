## Context

See proposal.md for the motivation. `ConsoleDock.jsx` currently passes one stack beginning with "SF Mono" to every xterm at 13px. In Ubuntu 22.04 with WebKitGTK 2.50.4, this stack measures `W=11px` and `i=4px` in both canvas and DOM. Even an installed later family does not help when the missing first family already resolved to a proportional substitute. A standalone `monospace` measures both as 7px, and explicit DejaVu Sans Mono measures both as 8px.

The temporary Linux font installation and alias used during diagnosis have been reverted. Runtime verification must retain that original environment.

## Goals / Non-Goals

**Goals:**
- Keep xterm's normal and bold ASCII glyphs on a uniform grid when preferred font names are unavailable or misresolved.
- Preserve existing font preference where a named family actually renders as monospaced.
- Resolve fonts before terminal creation using its owner document and configured size, including initially hidden tabs.

**Non-Goals:**
- Altering Linux configuration, distributing a replacement desktop theme, changing PTY transport, adding a font picker, or changing the terminal's size/line height.
- Replacing xterm's handling of CJK wide characters or changing non-console typography.

## Decisions

1. Probe each candidate separately, then pass only the selected family to xterm. Retaining a failing family at the head of a CSS list would reproduce the bug even when a valid family follows.
2. Use actual canvas advances for representative wide/narrow ASCII letters, digits, punctuation and spaces. Require finite positive equal widths within a small subpixel tolerance in normal and bold, with stable width between weights. Font-name availability checks alone do not prove the substitute is monospaced.
3. Preserve the existing named-family order, then try common Linux monospace families, ending with a standalone generic `monospace`. If measurement is unavailable or throws, use the generic family without preventing console startup. The browser owns generic-family fallback.
4. Measure per terminal construction, without retaining a font result across documents or later font availability changes. This avoids stale substitutions while keeping the check small and synchronous before xterm's first fit.
5. Keep selection in an isolated module with fake metric providers for deterministic behavior tests. Exercise the same module with actual xterm in Linux WebKit for the regression that mocks cannot establish.

## Risks / Trade-offs

- Floating-point/font hinting noise could reject a valid face → allow a bounded subpixel tolerance and verify fractional metrics.
- Bold substitution could still change grid width → validate bold and normal advances together.
- Candidate probes add startup work → use a small fixed ASCII sample, stop at the first valid face, and do not rerun during rendering.
- CJK glyphs may use another face → retain xterm's wide-character handling and verify mixed Chinese/ASCII output in the real renderer.

## Migration Plan

Ship through the normal frontend build and Desktop embedding path. No user migration is required. The local WSL package is installed in a separate versioned directory, with the official 0.9.27 directory retained. Local rollback restores the previous executable links and launcher icons recorded in the installation backup.


## Verification (2026-09-29)

- Focused Node font tests: 11 cases passed.
- Full frontend suite: `pnpm --dir web test` exited 0 (2,895 pass lines).
- Production frontend: `pnpm build` passed, including the generated-regex compatibility check.
- `openspec validate fix-console-font-fallback --strict` and `git diff --check` passed.
- Real xterm 5.5 / fit addon 0.10 in Ubuntu 22.04 WebKitGTK 2.50.4 reproduced the original font stack with normal `W=11, i=4` and bold `W=12, i=4`. The resolver selected a single `Consolas` request whose effective normal/bold ASCII advances were all 7px. The terminal grid changed from 11px to 7px.
- In the same runtime check, Chinese cells remained width 2, ASCII width 1, resizing changed 144 columns to 94, and input reached xterm's onData callback unchanged.
- Runtime bundle/output: `/tmp/acecode-console-font-runtime-tvpa53ih/` in WSL Ubuntu. Comparison screenshot: `C:/Users/shao/AppData/Local/WSL-XFCE/console-font-comparison.png`.
- The temporary Fira Code package and per-user SF Mono alias were removed before this verification; `fc-match 'SF Mono'` again returns the original Noto Sans CJK substitute. No system-font workaround is part of the delivered change.
- On the user's follow-up request, both native targets were built from the current checkout (HEAD `fbbc6997`) with MinSizeRel and WebKitGTK 4.1, using a separate WSL build directory and the unchanged verified frontend. The complete current frontend bytes were checked inside the daemon executable.
- Package: `acecode-0.9.27-console-font-fbbc6997-linux-x64.tar.gz`, 21,761,752 bytes, SHA-256 `26737753f6f493b4f401a7775875f05de6dcbe97f69e5890338cb85b234d6786`. Archive extraction, execution permissions, shared libraries, models.dev resolution (207 providers / 7,483 models) and all 111 seed files passed validation.
- Installed at `/opt/acecode/releases/0.9.27-console-font-fbbc6997`; both `/usr/local/bin` links and XFCE launcher icons now use that package. The original `/opt/acecode/releases/0.9.27` remains intact, with previous links/launchers recorded in `/var/backups/acecode-console-font-install-20260929-191743/`.
- Verified `/proc` paths for the running desktop shell and daemon both point to the new installation. The actual Desktop console was opened through its keyboard shortcut and its rendered text was inspected successfully, without a fontconfig alias or added font package. Screenshot: `C:/Users/shao/AppData/Local/WSL-XFCE/acecode-installed-console.png`.
- Installed binary SHA-256: CLI/daemon `b1529fddc58b18e605346ef491947b5bec0cc5a75d16454adfda31b7f7655a06`; Desktop `7799bd9089d09645f8c3612dc7a24cc638e1e7094905458d422dbaf6e28de0c5`. The displayed version remains 0.9.27 for this local build. User configuration was preserved.
