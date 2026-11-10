## Decisions

Use the existing global `summaryPanelCollapsed` preference in `acecode.uiPrefs.v1`. Treat an absent value as collapsed when reading older records. The previous version also wrote its defaults to storage, so a stored `false` can mean either the old default or a deliberate choice to open. Preserve both boolean values on upgrade.

The layout solver already determines whether an open summary fits beside the transcript. Use that decision to present the same preference as a docked panel or a popup. Keep other sidebar popups scoped to the active workbench. Route user dismissal of a floating summary through the preference setter so the toolbar, close button, and Escape agree after a reload.
