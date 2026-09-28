#include "activity_narrator.hpp"
#include "tool_preamble/tool_preamble.hpp"
#include <utility>

namespace acecode::agent {

void ActivityNarrator::set_config(const ToolPreambleConfig& cfg) {
    std::lock_guard<std::mutex> lk(tool_preamble_mu_);
    tool_preamble_cfg_ = cfg;
}

ToolPreambleConfig ActivityNarrator::config() const {
    std::lock_guard<std::mutex> lk(tool_preamble_mu_);
    return tool_preamble_cfg_;
}

bool ActivityNarrator::enabled() const {
    return config().enabled;
}

void ActivityNarrator::set_phase(const ToolPreambleTitle& preamble) {
    std::lock_guard<std::mutex> lk(tool_preamble_mu_);
    phase_preamble_ = preamble;
}

ToolPreambleTitle ActivityNarrator::phase() const {
    std::lock_guard<std::mutex> lk(tool_preamble_mu_);
    return phase_preamble_;
}

void ActivityNarrator::publish_phase(const ToolPreambleTitle& preamble,
                                       const ProgressEmitter& emit_progress) {
    if (preamble.title.empty()) return;
    set_phase(preamble);
    // 独立的 phase 键 + force,绕开进度节流:标题一出现 loading 就换文案,
    // 不用等下一条 reasoning / tool_planning。
    emit_progress("preamble", preamble.title, std::string{},
                  std::string{}, std::string{}, -1, true);
}

void ActivityNarrator::note_planned_tool(int tool_index, const std::string& native_name) {
    if (tool_index < 0 || native_name.empty()) return;
    std::lock_guard<std::mutex> lk(tool_preamble_mu_);
    const auto index = static_cast<std::size_t>(tool_index);
    if (step_planned_tools_.size() <= index) step_planned_tools_.resize(index + 1);
    step_planned_tools_[index] = native_name;
}

void ActivityNarrator::reset_step() {
    std::lock_guard<std::mutex> lk(tool_preamble_mu_);
    phase_preamble_ = {};
    step_planned_tools_.clear();
    current_batch_activity_ = {};
}

void ActivityNarrator::reset_turn() {
    std::lock_guard<std::mutex> lk(tool_preamble_mu_);
    phase_preamble_ = {};
    step_planned_tools_.clear();
    current_batch_activity_ = {};
    last_batch_tools_.clear();
    last_announced_activity_.clear();
}

void ActivityNarrator::announce(const std::string& label) {
    if (label.empty()) return;
    {
        std::lock_guard<std::mutex> lk(tool_preamble_mu_);
        if (label == last_announced_activity_) return;
        last_announced_activity_ = label;
    }
    if (callbacks_.on_thinking_title) callbacks_.on_thinking_title(label);
}

ToolPreambleTitle ActivityNarrator::for_phase(
    const std::string& phase) const {
    std::lock_guard<std::mutex> lk(tool_preamble_mu_);
    if (!tool_preamble_cfg_.enabled) return {};
    if (phase == "responding") {
        return {tool_preamble::kRespondingActivityLabel, tool_preamble::kSourceContext, ""};
    }
    const bool tool_phase = phase == "tool_running" || phase == "tool_planning";
    const bool waiting_phase =
        phase == "model_waiting" || phase == "reasoning" || phase == "preamble";
    if (!tool_phase && !waiting_phase) return {};
    if (phase == "tool_running" && !current_batch_activity_.title.empty()) {
        return current_batch_activity_;
    }
    if (tool_phase) {
        std::vector<std::string> planned;
        for (const auto& name : step_planned_tools_) {
            if (!name.empty()) planned.push_back(name);
        }
        if (!phase_preamble_.title.empty()) {
            ToolPreambleTitle out = phase_preamble_;
            out.kind = tool_preamble::batch_activity_kind(planned);
            return out;
        }
        if (!planned.empty()) {
            return {tool_preamble::batch_activity_label(planned),
                    tool_preamble::kSourceTemplate,
                    tool_preamble::batch_activity_kind(planned)};
        }
    }
    if (!phase_preamble_.title.empty()) return phase_preamble_;
    if (last_batch_tools_.empty()) {
        return {tool_preamble::kInitialActivityLabel, tool_preamble::kSourceContext, ""};
    }
    return {tool_preamble::after_batch_activity_label(last_batch_tools_),
            tool_preamble::kSourceContext, ""};
}

ToolPreambleTitle ActivityNarrator::resolve_step(
    const ChatResponse& accumulated) {
    std::lock_guard<std::mutex> lk(tool_preamble_mu_);
    if (!tool_preamble_cfg_.enabled || accumulated.tool_calls.empty()) return {};
    std::vector<std::string> names;
    names.reserve(accumulated.tool_calls.size());
    for (const auto& tc : accumulated.tool_calls) names.push_back(tc.function_name);
    ToolPreambleTitle out;
    out.kind = tool_preamble::batch_activity_kind(names);
    const ToolPreambleTitle title = phase_preamble_;
    if (!title.title.empty()) {
        out.title = title.title;
        out.source = tool_preamble::kSourceReasoning;
    } else {
        out.title = tool_preamble::batch_activity_label(names);
        out.source = tool_preamble::kSourceTemplate;
    }
    last_batch_tools_ = std::move(names);
    current_batch_activity_ = out;
    return out;
}

ToolPreambleTitle ActivityNarrator::reasoning_title(const std::string& reasoning) {
    const auto bold = tool_preamble::extract_first_bold_span(reasoning);
    if (bold.empty()) return {};
    const auto title = llm::normalize_title_line(
        bold, tool_preamble::kReasoningTitleMaxCodePoints);
    return title.empty() ? ToolPreambleTitle{} :
        ToolPreambleTitle{title, tool_preamble::kSourceReasoning, ""};
}

const char* ActivityNarrator::responding_label() {
    return tool_preamble::kRespondingActivityLabel;
}

} // namespace acecode::agent
