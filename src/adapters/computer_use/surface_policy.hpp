#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace acecode::computer_use::surface_policy {

enum class Relation { root, owned, menu, combo };

struct Evidence {
    std::uintptr_t root = 0;
    std::uintptr_t candidate = 0;
    std::uint32_t root_pid = 0;
    std::uint32_t candidate_pid = 0;
    bool visible = false;
    bool owner_chain_same_process = false;
    std::vector<std::uintptr_t> owner_chain;
    bool standard_menu = false;
    bool menu_active = false;
    bool menu_owner_in_root_tree = false;
    bool menu_thread_matches = false;
    bool combo_list = false;
    bool exact_combo_list_match = false;
};

inline std::optional<Relation> classify(const Evidence& evidence) {
    if (!evidence.visible || !evidence.root || !evidence.candidate || !evidence.root_pid
        || evidence.root_pid != evidence.candidate_pid) return std::nullopt;
    if (evidence.root == evidence.candidate) return Relation::root;
    // OS popup classes require their specific semantic evidence. Same process
    // or thread alone cannot prove that a menu belongs to the requested window.
    if (evidence.standard_menu)
        return evidence.menu_active && evidence.menu_owner_in_root_tree && evidence.menu_thread_matches
            ? std::optional<Relation>(Relation::menu) : std::nullopt;
    if (evidence.combo_list)
        return evidence.exact_combo_list_match ? std::optional<Relation>(Relation::combo) : std::nullopt;
    if (evidence.owner_chain_same_process
        && std::find(evidence.owner_chain.begin(), evidence.owner_chain.end(), evidence.root) != evidence.owner_chain.end())
        return Relation::owned;
    return std::nullopt;
}

inline const char* name(Relation relation) {
    switch (relation) {
        case Relation::root: return "root";
        case Relation::owned: return "owned_window";
        case Relation::menu: return "menu";
        case Relation::combo: return "combo_list";
    }
    return "unknown";
}

// -1 is unknown/missing, -2 is ambiguous. Screenshot ids are scoped to the
// current observation, so a missing id is safe only for a single captured image.
inline int select_screenshot(const std::vector<std::string>& ids, const std::optional<std::string>& requested) {
    // A secondary image must never silently replace an unavailable main image.
    if (!requested && (ids.empty() || ids.front().empty())) return -1;
    int selected = -1;
    for (std::size_t index = 0; index < ids.size(); ++index) {
        if (ids[index].empty()) continue;
        if (requested && ids[index] != *requested) continue;
        if (selected != -1) return -2;
        selected = static_cast<int>(index);
    }
    return selected;
}

} // namespace acecode::computer_use::surface_policy
