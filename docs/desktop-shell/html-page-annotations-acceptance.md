# HTML page annotation acceptance

Specification: [#193](https://github.com/tmoonlight/acecode/issues/193). Implementation tickets: #194–201. OpenSpec: `add-html-page-annotations`. Integration baseline: `0155a863`.

This is the execution checklist, not a claim that the feature is complete. Keep the pull request draft until both platform workflows and required automated checks have passed. Reference-app screenshots and source inspection do not qualify as ACECode acceptance.

## Automated checks

| Check | Result |
| --- | --- |
| OpenSpec strict validation | Passed for initial planning artifacts, 2026-11-18 |
| Annotation interaction through conversation submission | Pending implementation |
| All property previews, text-node identity and saved/draft layering | Pending implementation |
| Batch freeze, queue, receipt, receiver deduplication and failures | Pending implementation |
| Navigation, removed target, document ownership and untrusted commands | Pending implementation |
| Source-selection annotation regressions | Pending |
| Full frontend test suite and production build | Pending |
| Windows native build and focused tests | Pending |
| macOS native build and focused tests | Not executed; macOS environment required |
| Standards and specification code review | Pending |

## Real desktop workflow

Run the following independently on Windows and macOS against the integrated build. Record build commit, OS, commands, observations and evidence paths. Use an already-loaded local-service page and a remote page, with nested text elements and a long page containing recognizable content below the viewport. A runtime test or successful native API return is not sufficient proof of screenshot pixels.

| Step | Windows | macOS |
| --- | --- | --- |
| Toolbar enables mode; element click outlines; control opens trusted popup above browser | Not executed | Not executed |
| Text editing preserves nested child elements and their event behavior | Not executed | Not executed |
| Text color, background, opacity, font, font size and weight preview | Not executed | Not executed |
| Radius, border color and width, width, height, spacing, padding and margin preview | Not executed | Not executed |
| Ratio lock starts off and captures current ratio when enabled | Not executed | Not executed |
| Unchanged values produce no differences; cancel preserves previous saved preview on same element | Not executed | Not executed |
| Multiple elements and one replaceable region share description and pin; region has no property editing | Not executed | Not executed |
| Save produces pin/count but no model work and no HTML/CSS file writes | Not executed | Not executed |
| Original comparison keeps pins; restoring previews and leaving mode retain saved work | Not executed | Not executed |
| Explicit batch send yields one message and exactly one full-page image with below-viewport pixels | Not executed | Not executed |
| Busy model receives queued new turn, not steering; composer draft and attachments remain | Not executed | Not executed |
| Session switching cannot retarget frozen batch; additions during send remain pending | Not executed | Not executed |
| Accepted batch clears only its pending count, retains pins/previews, and subsequent numbers increase | Not executed | Not executed |
| Capture/upload failure sends nothing; uncertain receipt retries same identity without duplicate work | Not executed | Not executed |
| Navigation/refresh defaults to staying; replacement/removal invalidates without relocation | Not executed | Not executed |
| No usable conversation permits annotation but blocks sending without creating a conversation | Not executed | Not executed |
| Existing source-selection annotation workflow remains functional | Not executed | Not executed |

## Recording failures

Keep failed and unavailable checks explicit. Do not mark a task complete because a platform implementation exists or because another platform passed. Invalid saved annotation records cannot be repaired by introducing an unapproved edit/delete UI. Capture resource limits must report failure rather than silently truncate. Runtime retention does not imply restart recovery.
