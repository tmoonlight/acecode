## Purpose

Keep sidebar session and workspace dragging tied to an actual mouse-button hold, recovering when a release event is not delivered to the page.

## ADDED Requirements

### Requirement: Released mouse movement cancels dragging
Session and workspace drag interactions SHALL cancel pending or active mouse drags when a movement reports that the primary mouse button is no longer pressed. Cancellation SHALL remove drag feedback without submitting a reordered list.

#### Scenario: Tap release was not delivered
- **WHEN** a mouse press starts a pending drag, no release event arrives, and a later movement has no primary button pressed
- **THEN** movement does not start or continue a drag and no order is saved

### Requirement: Normal input remains usable
The release guard SHALL preserve ordinary dragging while the primary mouse button remains pressed and SHALL NOT infer release from touch, pen, or events with unavailable button state. Cancelling a stale workspace drag SHALL preserve the next ordinary click.

#### Scenario: Real drag and subsequent click
- **WHEN** the user holds the primary mouse button while dragging
- **THEN** the existing drag threshold and reorder behavior remain available
- **WHEN** a stale workspace drag is cancelled and the user clicks a workspace
- **THEN** that click is handled normally
