#include "agent_browser_state_json.hpp"

namespace acecode::desktop {

nlohmann::json agent_browser_state_json(const AgentBrowserState& state) {
    nlohmann::json result{
        {"supported", state.supported},
        {"ready", state.ready},
        {"loading", state.loading},
        {"visible", state.visible},
        {"active", state.active},
        {"closed", state.closed},
        {"can_go_back", state.can_go_back},
        {"can_go_forward", state.can_go_forward},
        {"shared_with_agent", state.shared_with_agent},
        {"element_selection_active", state.element_selection_active},
        {"element_selection_serial", state.element_selection_serial},
        {"url", state.url},
        {"title", state.title},
        {"favicon", state.favicon},
        {"content_state", state.content_state},
        {"failure_kind", state.failure_kind},
        {"error", state.error},
        {"diagnostic", state.diagnostic},
        {"page_id", state.page_id},
        {"owner", agent_browser_page_owner_json(state.owner)},
        {"agent_target", state.agent_target},
    };
    if (!state.selected_element_json.empty()) {
        const auto selected = nlohmann::json::parse(
            state.selected_element_json, nullptr, false);
        if (!selected.is_discarded()) result["selected_element"] = selected;
    }
    if (!state.page_annotations_json.empty()) {
        const auto annotations = nlohmann::json::parse(state.page_annotations_json, nullptr, false);
        if (annotations.is_object()) result["annotations"] = annotations;
    }
    return result;
}

}
