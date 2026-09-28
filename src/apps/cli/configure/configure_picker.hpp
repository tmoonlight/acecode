#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace acecode {

// A single row in a picker list. `label` is the primary identifier (e.g. a
// provider id or a model id) that the user looks for; `secondary` is the
// dimmer metadata shown to the right (ctx window, cost, capability tags).
struct PickerItem {
    std::string label;
    std::string secondary;
};

struct PickerOptions {
    std::string title;          // header line at the top of the picker
    std::string hint;            // optional override for the key-legend footer
    std::size_t default_index = 0; // initial highlight position
    std::size_t page_size = 30;  // rows per PageUp/PageDown jump
    bool allow_custom = false;   // when true, exposes a "<Custom ...>" entry
    bool search_as_you_type = false; // always-visible input; typing filters rows
    std::string search_placeholder = "Type to search";
};

struct PickerResult {
    bool cancelled = false;
    bool custom = false;                 // user picked the "<Custom ...>" escape
    std::size_t index = 0;               // index into the ORIGINAL items vector
};

// Drive an interactive single-selection picker. On a TTY the FTXUI path runs
// (Up/Down, PageUp/PageDown, Home/End, Enter, digits, `/` filter, Esc, `c`).
// When stdout is not a TTY, the helper falls back to a plain-stdin numbered
// flow matching the pre-change behavior of configure_catalog.cpp so scripted
// callers do not regress.
PickerResult run_ftxui_picker(const std::vector<PickerItem>& items,
                              const PickerOptions& opts);

// Return original item indices whose label or secondary text contains `query`
// case-insensitively. Order is stable so the first matching row is a
// deterministic autocomplete suggestion.
std::vector<std::size_t> filter_picker_items(
    const std::vector<PickerItem>& items,
    const std::string& query);

// Pure row-rendering helper. Returns a single-line string combining `label`,
// a two-space gutter, and `secondary`, truncated to `width` bytes. When the
// label on its own is already >= width the label is returned unchanged (never
// clipped) and secondary is dropped. When the full thing does not fit, the
// secondary is truncated and suffixed with `...`. With an empty secondary the
// return value is the label alone with no trailing whitespace.
std::string format_picker_row(const std::string& label,
                              const std::string& secondary,
                              std::size_t width);

} // namespace acecode
