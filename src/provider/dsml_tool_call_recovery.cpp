#include "dsml_tool_call_recovery.hpp"

#include "tool/tool_protocol_names.hpp"
#include "utils/sha1.hpp"
#include "utils/uuid.hpp"

#include <array>
#include <cctype>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace acecode {
namespace {

constexpr std::string_view kToolCallsOpen = u8"<｜DSML｜tool_calls>";
constexpr std::string_view kToolCallsClose = u8"</｜DSML｜tool_calls>";
constexpr std::string_view kInvokeOpenPrefix = u8"<｜DSML｜invoke name=\"";
constexpr std::string_view kInvokeClose = u8"</｜DSML｜invoke>";
constexpr std::string_view kParameterOpenPrefix = u8"<｜DSML｜parameter name=\"";
constexpr std::string_view kParameterStringAttribute = " string=\"";
constexpr std::string_view kParameterClose = u8"</｜DSML｜parameter>";
constexpr std::string_view kEndOfSentence = u8"<｜end▁of▁sentence｜>";
constexpr std::string_view kBeginOfSentence = u8"<｜begin▁of▁sentence｜>";

constexpr std::array<std::string_view, 2> kDropTokens{{
    kEndOfSentence,
    kBeginOfSentence,
}};

bool starts_with_at(std::string_view input,
                    std::size_t position,
                    std::string_view value) {
    return position <= input.size() &&
           value.size() <= input.size() - position &&
           input.compare(position, value.size(), value) == 0;
}

void skip_ascii_whitespace(std::string_view input, std::size_t& position) {
    while (position < input.size() &&
           std::isspace(static_cast<unsigned char>(input[position]))) {
        ++position;
    }
}

std::string_view trailing_after_wrapper(std::string_view candidate) {
    const std::size_t close_at = candidate.find(kToolCallsClose);
    if (close_at == std::string_view::npos) return {};

    std::size_t position = close_at + kToolCallsClose.size();
    if (starts_with_at(candidate, position, kEndOfSentence)) {
        position += kEndOfSentence.size();
    } else if (starts_with_at(candidate, position, kBeginOfSentence)) {
        position += kBeginOfSentence.size();
    }
    return candidate.substr(position);
}

DsmlToolCallRecoveryResult reject_candidate(std::string_view candidate,
                                            std::string error) {
    DsmlToolCallRecoveryResult result;
    // Never surface raw DSML markup. Keep only text after a complete wrapper.
    result.visible_text = std::string(trailing_after_wrapper(candidate));
    result.error = std::move(error);
    return result;
}

std::string synthesize_dsml_tool_call_id(const std::string& id_scope,
                                         std::size_t index,
                                         const std::string& name,
                                         const std::string& arguments) {
    std::string fingerprint = id_scope;
    fingerprint.push_back('\n');
    fingerprint.append(std::to_string(index));
    fingerprint.push_back('\n');
    fingerprint.append(name);
    fingerprint.push_back('\n');
    fingerprint.append(arguments);
    return "call_dsml_" + sha1_hex(fingerprint).substr(0, 24);
}

bool is_known_tool_name(const std::unordered_set<std::string>& allowed_tools,
                        const std::string& name) {
    if (allowed_tools.empty()) return false;
    if (allowed_tools.find(name) != allowed_tools.end()) return true;
    return false;
}

DsmlToolCallRecoveryResult parse_dsml_candidate(
    std::string_view candidate,
    const std::unordered_set<std::string>& allowed_tools,
    const std::string& id_scope) {
    if (allowed_tools.empty()) {
        return reject_candidate(candidate, "DSML recovery is disabled without tools");
    }
    if (!starts_with_at(candidate, 0, kToolCallsOpen)) {
        return reject_candidate(candidate, "missing DSML tool_calls start marker");
    }

    std::size_t position = kToolCallsOpen.size();
    skip_ascii_whitespace(candidate, position);

    std::vector<ToolCall> calls;
    while (starts_with_at(candidate, position, kInvokeOpenPrefix)) {
        position += kInvokeOpenPrefix.size();
        const std::size_t tool_name_end = candidate.find("\"", position);
        if (tool_name_end == std::string_view::npos || tool_name_end == position) {
            return reject_candidate(candidate, "invalid DSML invoke name");
        }
        if (!starts_with_at(candidate, tool_name_end, "\">")) {
            return reject_candidate(candidate, "invalid DSML invoke name");
        }

        std::string tool_name(candidate.substr(position, tool_name_end - position));
        if (!is_known_tool_name(allowed_tools, tool_name)) {
            return reject_candidate(candidate, "DSML invoke references an unknown tool");
        }
        position = tool_name_end + 2;
        skip_ascii_whitespace(candidate, position);

        nlohmann::json arguments = nlohmann::json::object();
        std::unordered_set<std::string> parameter_names;
        while (starts_with_at(candidate, position, kParameterOpenPrefix)) {
            position += kParameterOpenPrefix.size();
            const std::size_t name_end = candidate.find('"', position);
            if (name_end == std::string_view::npos || name_end == position) {
                return reject_candidate(candidate, "invalid DSML parameter name");
            }

            std::string parameter_name(
                candidate.substr(position, name_end - position));
            if (!parameter_names.insert(parameter_name).second) {
                return reject_candidate(candidate, "duplicate DSML parameter name");
            }
            position = name_end + 1;

            bool is_string = true;
            if (starts_with_at(candidate, position, kParameterStringAttribute)) {
                position += kParameterStringAttribute.size();
                const std::size_t string_flag_end = candidate.find('"', position);
                if (string_flag_end == std::string_view::npos) {
                    return reject_candidate(candidate, "invalid DSML parameter string flag");
                }
                const std::string_view string_flag = candidate.substr(
                    position, string_flag_end - position);
                if (string_flag != "true" && string_flag != "false") {
                    return reject_candidate(candidate, "invalid DSML parameter string flag");
                }
                is_string = string_flag == "true";
                position = string_flag_end + 1;
            }

            if (!starts_with_at(candidate, position, ">")) {
                return reject_candidate(candidate, "invalid DSML parameter open tag");
            }
            position += 1;

            const std::size_t value_end = candidate.find(kParameterClose, position);
            if (value_end == std::string_view::npos) {
                return reject_candidate(candidate, "missing DSML parameter end marker");
            }
            const std::string_view raw_value = candidate.substr(
                position, value_end - position);

            if (is_string) {
                arguments[parameter_name] = std::string(raw_value);
            } else {
                auto parsed_value = nlohmann::json::parse(
                    raw_value.begin(), raw_value.end(), nullptr, false);
                if (parsed_value.is_discarded()) {
                    return reject_candidate(candidate, "invalid JSON DSML parameter value");
                }
                arguments[parameter_name] = std::move(parsed_value);
            }

            position = value_end + kParameterClose.size();
            skip_ascii_whitespace(candidate, position);
        }

        if (!starts_with_at(candidate, position, kInvokeClose)) {
            return reject_candidate(candidate, "missing DSML invoke end marker");
        }
        position += kInvokeClose.size();

        ToolCall call;
        call.function_name = std::move(tool_name);
        call.function_arguments = arguments.dump();
        call.id = synthesize_dsml_tool_call_id(
            id_scope, calls.size(), call.function_name, call.function_arguments);
        calls.push_back(std::move(call));

        skip_ascii_whitespace(candidate, position);
    }

    if (calls.empty()) {
        return reject_candidate(candidate, "DSML tool_calls wrapper is empty");
    }
    if (!starts_with_at(candidate, position, kToolCallsClose)) {
        return reject_candidate(candidate, "missing DSML tool_calls end marker");
    }
    position += kToolCallsClose.size();
    if (starts_with_at(candidate, position, kEndOfSentence)) {
        position += kEndOfSentence.size();
    } else if (starts_with_at(candidate, position, kBeginOfSentence)) {
        position += kBeginOfSentence.size();
    }

    DsmlToolCallRecoveryResult result;
    result.recovered = true;
    result.tool_calls = std::move(calls);
    if (position < candidate.size()) {
        result.visible_text.assign(
            candidate.data() + position, candidate.size() - position);
    }
    return result;
}

bool is_prefix_of(std::string_view token, std::string_view probe) {
    return probe.size() <= token.size() &&
           token.compare(0, probe.size(), probe) == 0;
}

bool probe_matches_known_prefix(std::string_view probe) {
    if (probe.empty()) return false;
    if (is_prefix_of(kToolCallsOpen, probe)) return true;
    for (std::string_view token : kDropTokens) {
        if (is_prefix_of(token, probe)) return true;
    }
    return false;
}

bool probe_is_drop_token(std::string_view probe) {
    for (std::string_view token : kDropTokens) {
        if (probe == token) return true;
    }
    return false;
}

void add_allowed_tool_name(std::unordered_set<std::string>& allowed,
                           const std::string& name) {
    if (name.empty()) return;
    allowed.insert(name);
    allowed.insert(model_tool_name_for_native(name));
    if (auto native = native_tool_name_for_public_alias(name)) {
        allowed.insert(*native);
    }
}

} // namespace

DsmlToolCallStreamFilter::DsmlToolCallStreamFilter(
    const std::vector<ToolDef>& tools) {
    for (const auto& tool : tools) {
        add_allowed_tool_name(allowed_tools_, tool.name);
    }
    reset();
}

DsmlToolCallStreamFilter::DsmlToolCallStreamFilter(
    std::unordered_set<std::string> allowed_tools)
    : allowed_tools_(std::move(allowed_tools)) {
    reset();
}

std::string DsmlToolCallStreamFilter::push(std::string_view chunk) {
    std::string output;
    output.reserve(chunk.size());
    std::size_t position = 0;
    while (position < chunk.size()) {
        if (capturing_) {
            candidate_.append(chunk.data() + position, chunk.size() - position);
            break;
        }

        const char c = chunk[position];
        if (!marker_probe_.empty()) {
            std::string next = marker_probe_;
            next.push_back(c);
            if (probe_matches_known_prefix(next)) {
                marker_probe_.push_back(c);
                ++position;
                if (marker_probe_ == kToolCallsOpen) {
                    if (marker_can_start_here()) {
                        candidate_ = std::move(marker_probe_);
                        marker_probe_.clear();
                        capturing_ = true;
                    } else {
                        for (char probe_byte : marker_probe_) {
                            append_visible_byte(probe_byte, output);
                        }
                        marker_probe_.clear();
                    }
                } else if (probe_is_drop_token(marker_probe_)) {
                    marker_probe_.clear();
                }
                continue;
            }

            for (char probe_byte : marker_probe_) {
                append_visible_byte(probe_byte, output);
            }
            marker_probe_.clear();
            continue;
        }

        if (c == '<' && probe_matches_known_prefix(std::string_view(&c, 1))) {
            marker_probe_.push_back(c);
            ++position;
            continue;
        }

        append_visible_byte(c, output);
        ++position;
    }
    return output;
}

DsmlToolCallRecoveryResult DsmlToolCallStreamFilter::finish() {
    if (capturing_) {
        DsmlToolCallRecoveryResult aggregate;
        const auto merge_candidate = [&aggregate](
            DsmlToolCallRecoveryResult candidate_result) {
            std::string remainder = std::move(candidate_result.visible_text);
            aggregate.recovered = aggregate.recovered || candidate_result.recovered;
            for (auto& call : candidate_result.tool_calls) {
                aggregate.tool_calls.push_back(std::move(call));
            }
            if (!candidate_result.error.empty()) {
                if (!aggregate.error.empty()) aggregate.error.append("; ");
                aggregate.error.append(candidate_result.error);
            }
            return remainder;
        };

        std::string remainder = merge_candidate(
            parse_dsml_candidate(candidate_, allowed_tools_, id_scope_));
        marker_probe_.clear();
        candidate_.clear();
        capturing_ = false;

        // parse_dsml_candidate returns the raw suffix after one complete
        // wrapper. Scan that suffix again instead of exposing it directly:
        // gateways may insert whitespace before a sentence token or emit more
        // than one wrapper in a response.
        while (!remainder.empty()) {
            DsmlToolCallStreamFilter tail_filter(allowed_tools_);
            aggregate.visible_text += tail_filter.push(remainder);
            if (!tail_filter.capturing_) {
                auto tail = tail_filter.finish();
                aggregate.visible_text += tail.visible_text;
                if (!tail.error.empty()) {
                    if (!aggregate.error.empty()) aggregate.error.append("; ");
                    aggregate.error.append(tail.error);
                }
                break;
            }

            remainder = merge_candidate(parse_dsml_candidate(
                tail_filter.candidate_,
                tail_filter.allowed_tools_,
                tail_filter.id_scope_));
        }
        return aggregate;
    }

    DsmlToolCallRecoveryResult result;
    // Incomplete special-token prefix at end of stream: hide it rather than
    // flash `<｜DSML｜...` leftovers as assistant text.
    if (!marker_probe_.empty() && !probe_matches_known_prefix(marker_probe_)) {
        result.visible_text = std::move(marker_probe_);
    }
    marker_probe_.clear();
    return result;
}

void DsmlToolCallStreamFilter::reset() {
    id_scope_ = generate_uuid_v7();
    marker_probe_.clear();
    candidate_.clear();
    capturing_ = false;
    in_fence_ = false;
    fence_char_ = '\0';
    fence_length_ = 0;
    reset_markdown_line();
}

void DsmlToolCallStreamFilter::append_visible_byte(char c,
                                                   std::string& output) {
    output.push_back(c);
    update_fence_state(c);
}

void DsmlToolCallStreamFilter::update_fence_state(char c) {
    if (c == '\n') {
        finish_markdown_line();
        return;
    }

    if (line_fence_run_active_) {
        if (c == line_fence_char_) {
            ++line_fence_run_;
            if (!in_fence_ && line_fence_run_ >= 3) {
                line_opening_fence_ = true;
            }
            return;
        }

        line_fence_run_active_ = false;
        if (!std::isspace(static_cast<unsigned char>(c))) {
            line_nonspace_after_fence_ = true;
        }
        return;
    }

    if (!line_prefix_active_) {
        if (!std::isspace(static_cast<unsigned char>(c))) {
            line_nonspace_after_fence_ = true;
        }
        return;
    }

    if (c == ' ' && line_leading_spaces_ < 3) {
        ++line_leading_spaces_;
        return;
    }

    if (c == '`' || c == '~') {
        line_prefix_active_ = false;
        line_fence_run_active_ = true;
        line_fence_char_ = c;
        line_fence_run_ = 1;
        return;
    }

    line_prefix_active_ = false;
    if (!std::isspace(static_cast<unsigned char>(c))) {
        line_nonspace_after_fence_ = true;
    }
}

void DsmlToolCallStreamFilter::finish_markdown_line() {
    if (!in_fence_) {
        if (line_opening_fence_) {
            in_fence_ = true;
            fence_char_ = line_fence_char_;
            fence_length_ = line_fence_run_;
        }
    } else if (line_fence_char_ == fence_char_ &&
               line_fence_run_ >= fence_length_ &&
               !line_nonspace_after_fence_) {
        in_fence_ = false;
        fence_char_ = '\0';
        fence_length_ = 0;
    }
    reset_markdown_line();
}

void DsmlToolCallStreamFilter::reset_markdown_line() {
    line_leading_spaces_ = 0;
    line_prefix_active_ = true;
    line_fence_run_active_ = false;
    line_fence_char_ = '\0';
    line_fence_run_ = 0;
    line_opening_fence_ = false;
    line_nonspace_after_fence_ = false;
}

bool DsmlToolCallStreamFilter::marker_can_start_here() const {
    return !in_fence_ && !line_opening_fence_;
}

DsmlToolCallRecoveryResult recover_dsml_tool_calls(
    std::string_view text,
    const std::vector<ToolDef>& tools) {
    DsmlToolCallStreamFilter filter(tools);
    DsmlToolCallRecoveryResult result;
    result.visible_text = filter.push(text);

    auto tail = filter.finish();
    result.visible_text += tail.visible_text;
    result.recovered = tail.recovered;
    result.tool_calls = std::move(tail.tool_calls);
    result.error = std::move(tail.error);
    return result;
}

} // namespace acecode
