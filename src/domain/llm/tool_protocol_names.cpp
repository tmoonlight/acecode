#include "tool_protocol_names.hpp"

#include "llm/llm_provider.hpp"

#include <memory>
#include <mutex>
#include <unordered_set>
#include <utility>

namespace acecode {

namespace {

const ToolProtocolNameMappings kSeedMappings{
    {"file_write", "write"},
    {"file_edit", "edit"},
    {"file_read", "read"},
    {"TodoWrite", "todowrite"},
};

// 生效映射是进程级共享状态:读多写少(每次构造请求读若干次,只有设置保存
// 时才写),用 shared_ptr 快照换出,读者拿到的向量在其生命周期内不会被改。
std::mutex& mappings_mutex() {
    static std::mutex mu;
    return mu;
}

std::shared_ptr<const ToolProtocolNameMappings>& mappings_slot() {
    static std::shared_ptr<const ToolProtocolNameMappings> slot =
        std::make_shared<const ToolProtocolNameMappings>();
    return slot;
}

std::shared_ptr<const ToolProtocolNameMappings> current_mappings() {
    std::lock_guard<std::mutex> lk(mappings_mutex());
    return mappings_slot();
}

void set_error(std::string* error, const std::string& value) {
    if (error) *error = value;
}

bool is_name_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_';
}

void rewrite_tool_call_for_model(nlohmann::json& tool_call,
                                 const ToolProtocolNameMappings& mappings) {
    if (!tool_call.is_object()) return;
    auto function_it = tool_call.find("function");
    if (function_it == tool_call.end() || !function_it->is_object()) return;
    auto name_it = function_it->find("name");
    if (name_it == function_it->end() || !name_it->is_string()) return;
    const std::string native = name_it->get<std::string>();
    for (const auto& mapping : mappings) {
        if (mapping.native_name == native) {
            *name_it = mapping.public_name;
            return;
        }
    }
}

// 递归重写 JSON Schema 里所有 "description" 字符串;其它键原样保留。
void rewrite_schema_descriptions(nlohmann::json& node,
                                 const ToolProtocolNameMappings& mappings) {
    if (node.is_object()) {
        for (auto it = node.begin(); it != node.end(); ++it) {
            if (it.key() == "description" && it.value().is_string()) {
                it.value() = rewrite_model_facing_text(
                    it.value().get<std::string>(), mappings);
            } else {
                rewrite_schema_descriptions(it.value(), mappings);
            }
        }
    } else if (node.is_array()) {
        for (auto& item : node) rewrite_schema_descriptions(item, mappings);
    }
}

} // namespace

const ToolProtocolNameMappings& default_model_tool_name_mappings() {
    return kSeedMappings;
}

ToolProtocolNameMappings model_tool_name_mappings() {
    return *current_mappings();
}

bool set_model_tool_name_mappings(ToolProtocolNameMappings mappings,
                                  std::string* error) {
    if (!validate_model_tool_name_mappings(mappings, error)) return false;
    auto published =
        std::make_shared<const ToolProtocolNameMappings>(std::move(mappings));
    std::lock_guard<std::mutex> lk(mappings_mutex());
    mappings_slot() = std::move(published);
    if (error) error->clear();
    return true;
}

ScopedModelToolNameMappings::ScopedModelToolNameMappings(
    ToolProtocolNameMappings mappings)
    : previous_(model_tool_name_mappings()) {
    set_model_tool_name_mappings(std::move(mappings));
}

ScopedModelToolNameMappings::ScopedModelToolNameMappings(
    std::initializer_list<ToolProtocolNameMapping> mappings)
    : ScopedModelToolNameMappings(
          ToolProtocolNameMappings(mappings.begin(), mappings.end())) {}

ScopedModelToolNameMappings::~ScopedModelToolNameMappings() {
    set_model_tool_name_mappings(previous_);
}

bool is_valid_model_tool_name(std::string_view name) {
    if (name.empty() || name.size() > 64) return false;
    for (char c : name) {
        if (!is_name_char(c) && c != '-') return false;
    }
    return true;
}

std::string model_tool_name_for_native(std::string_view native_name) {
    const auto mappings = current_mappings();
    for (const auto& mapping : *mappings) {
        if (mapping.native_name == native_name) return mapping.public_name;
    }
    return std::string(native_name);
}

std::optional<std::string> native_tool_name_for_public_alias(
    std::string_view public_name) {
    const auto mappings = current_mappings();
    for (const auto& mapping : *mappings) {
        if (mapping.public_name == public_name) return mapping.native_name;
    }
    return std::nullopt;
}

bool ascii_iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char x = a[i];
        char y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

std::optional<std::string> native_tool_name_for_public_alias_ci(
    std::string_view public_name) {
    if (public_name.empty()) return std::nullopt;
    const auto mappings = current_mappings();
    std::optional<std::string> found;
    for (const auto& mapping : *mappings) {
        if (!ascii_iequals(mapping.public_name, public_name)) continue;
        if (found && *found != mapping.native_name) return std::nullopt;
        found = mapping.native_name;
    }
    return found;
}

bool validate_model_tool_name_mappings(const ToolProtocolNameMappings& mappings,
                                       std::string* error) {
    std::unordered_set<std::string> native_names;
    std::unordered_set<std::string> public_names;
    for (const auto& mapping : mappings) {
        if (mapping.native_name.empty() || mapping.public_name.empty()) {
            set_error(error, "tool protocol names must not be empty");
            return false;
        }
        if (!is_valid_model_tool_name(mapping.public_name)) {
            set_error(error, "public tool protocol name '" + mapping.public_name +
                                 "' must match ^[A-Za-z0-9_-]{1,64}$");
            return false;
        }
        if (mapping.native_name == mapping.public_name) {
            set_error(error, "tool protocol mapping must change the native name '" +
                                 mapping.native_name + "'");
            return false;
        }
        if (!native_names.emplace(mapping.native_name).second) {
            set_error(error, "duplicate native tool protocol name '" +
                                 mapping.native_name + "'");
            return false;
        }
        if (!public_names.emplace(mapping.public_name).second) {
            set_error(error, "duplicate public tool protocol name '" +
                                 mapping.public_name + "'");
            return false;
        }
    }

    for (const auto& mapping : mappings) {
        if (native_names.count(mapping.public_name) != 0) {
            set_error(error, "public tool protocol name '" + mapping.public_name +
                                 "' collides with a mapped native name");
            return false;
        }
    }

    if (error) error->clear();
    return true;
}

bool validate_model_tool_name_mappings(std::string* error) {
    return validate_model_tool_name_mappings(*current_mappings(), error);
}

std::string rewrite_model_facing_text(std::string text) {
    return rewrite_model_facing_text(std::move(text), *current_mappings());
}

std::string rewrite_model_facing_text(std::string text,
                                      const ToolProtocolNameMappings& mappings) {
    if (mappings.empty() || text.empty()) return text;
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        const bool boundary_before = i == 0 || !is_name_char(text[i - 1]);
        const ToolProtocolNameMapping* best = nullptr;
        if (boundary_before) {
            for (const auto& mapping : mappings) {
                const auto& native = mapping.native_name;
                if (native.empty() || text.compare(i, native.size(), native) != 0) {
                    continue;
                }
                const std::size_t end = i + native.size();
                if (end < text.size() && is_name_char(text[end])) continue;
                if (!best || native.size() > best->native_name.size()) best = &mapping;
            }
        }
        if (best) {
            out += best->public_name;
            i += best->native_name.size();
        } else {
            out.push_back(text[i]);
            ++i;
        }
    }
    return out;
}

bool translate_tool_definitions_for_model(
    const std::vector<ToolDef>& native_definitions,
    std::vector<ToolDef>& model_definitions,
    std::string* error) {
    const auto mappings = current_mappings();
    if (!validate_model_tool_name_mappings(*mappings, error)) return false;

    std::vector<ToolDef> translated;
    translated.reserve(native_definitions.size());
    std::unordered_set<std::string> emitted_names;
    for (const auto& native_definition : native_definitions) {
        ToolDef definition = native_definition;
        definition.name = model_tool_name_for_native(native_definition.name);
        if (!emitted_names.emplace(definition.name).second) {
            set_error(error, "duplicate model-facing tool name '" +
                                 definition.name + "'");
            return false;
        }
        if (!mappings->empty()) {
            definition.description =
                rewrite_model_facing_text(definition.description, *mappings);
            rewrite_schema_descriptions(definition.parameters, *mappings);
        }
        translated.push_back(std::move(definition));
    }

    model_definitions = std::move(translated);
    if (error) error->clear();
    return true;
}

void rewrite_tool_calls_for_model(ChatMessage& message) {
    const auto mappings = current_mappings();
    if (mappings->empty()) return;
    if (message.tool_calls.is_array()) {
        for (auto& tool_call : message.tool_calls) {
            rewrite_tool_call_for_model(tool_call, *mappings);
        }
    } else if (message.tool_calls.is_object()) {
        rewrite_tool_call_for_model(message.tool_calls, *mappings);
    }
}

void rewrite_tool_calls_for_model(std::vector<ChatMessage>& messages) {
    const auto mappings = current_mappings();
    if (mappings->empty()) return;
    for (auto& message : messages) {
        if (message.tool_calls.is_array()) {
            for (auto& tool_call : message.tool_calls) {
                rewrite_tool_call_for_model(tool_call, *mappings);
            }
        } else if (message.tool_calls.is_object()) {
            rewrite_tool_call_for_model(message.tool_calls, *mappings);
        }
    }
}

} // namespace acecode
