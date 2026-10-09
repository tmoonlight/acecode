## MODIFIED Requirements

### Requirement: Busy input shows interrupt and queue actions
The input bar SHALL expose one primary icon button while the current session is busy. Its action SHALL be stop when the draft has no submittable content and queue when the draft contains text or other submittable content. The button SHALL retain its position, dimensions, and theme-accent background across these states, with an accessible action label and tooltip.

#### Scenario: Busy input with text
- **WHEN** the current session is busy and the input contains non-whitespace text
- **THEN** the input action area SHALL show only one primary button with a queue SVG icon
- **AND** clicking that button or pressing Enter SHALL submit the draft through the existing queue path
- **AND** blocking-input and submission-in-progress guards SHALL prevent duplicate or disallowed submissions

#### Scenario: Busy input without text
- **WHEN** the current session is busy and the draft is empty or whitespace-only with no submittable attachments or context
- **THEN** the primary button SHALL show a solid square stop icon on the theme-accent background
- **AND** clicking the button SHALL interrupt the current task
- **AND** pressing Enter in the empty editor SHALL NOT interrupt the task or enqueue an empty message

#### Scenario: Busy input with attachments only
- **WHEN** the current session is busy and the draft has no text but contains a submittable attachment, pasted text, or context
- **THEN** the primary button SHALL show the queue SVG icon and SHALL allow the existing attachment submission path

#### Scenario: Stop request is pending
- **WHEN** the draft is empty and a stop request is pending
- **THEN** the primary button SHALL keep its square icon and theme-accent background
- **AND** the button SHALL be disabled and announce that the task is stopping

#### Scenario: Draft changes during a busy turn
- **WHEN** the user enters content or removes all submittable content during a busy turn
- **THEN** the same primary button SHALL switch between queue and stop without adding another action button or shifting its position

#### Scenario: Idle input uses normal send action
- **WHEN** the current session is not busy
- **THEN** the primary button SHALL use the existing send action, or the existing resume action for an empty draft with a paused queue
- **AND** empty-draft retry eligibility and disabled/submitting guards SHALL remain effective

#### Scenario: Newline and composition behavior
- **WHEN** the user presses Shift+Enter or confirms an IME composition
- **THEN** the editor SHALL preserve its existing newline and composition handling without unintended queue or stop actions
