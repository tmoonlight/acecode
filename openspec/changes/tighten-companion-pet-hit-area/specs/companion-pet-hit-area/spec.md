## ADDED Requirements

### Requirement: Companion body activation follows visible art

The companion pet SHALL use visible sprite silhouette bands for native and page pointer hit detection, including while hovered. A small hover anchor MAY keep a stationary pointer active across animation frames without activating the full sprite frame.

#### Scenario: Hovering does not activate transparent side margins
- **WHEN** a pointer enters the visible companion pet
- **THEN** the pet responds to hover while transparent side margins outside its silhouette and small motion allowance remain click-through

### Requirement: Resize grip follows the pet corner

The resize grip SHALL retain a 16 CSS pixel target at the visible pet's upper left corner and update when the viewport, character or docked pose changes. Native cursor hit detection SHALL use the reported grip for companion pets.

#### Scenario: Character and size change
- **WHEN** the user changes companion character or window size
- **THEN** the grip is repositioned next to the visible pet instead of staying at the wide window's left edge

### Requirement: Session bubbles retain independent width

Session bubbles SHALL retain their existing width, shadow padding and independent native hit region when the companion body region is tightened.

#### Scenario: Wide session card above narrow pet
- **WHEN** a session card is displayed above a companion pet
- **THEN** both ends of the card remain visible and interactive while transparent space beside the body remains click-through
