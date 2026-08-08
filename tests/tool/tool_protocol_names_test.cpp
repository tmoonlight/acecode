#include <gtest/gtest.h>

#include "tool/tool_executor.hpp"
#include "tool/tool_protocol_names.hpp"

#include <atomic>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {

acecode::ToolImpl make_protocol_tool(
    std::string name,
    std::atomic<int>* calls = nullptr,
    std::string* captured_arguments = nullptr) {
    acecode::ToolImpl tool;
    tool.definition.name = std::move(name);
    tool.definition.description = "protocol test tool";
    tool.definition.parameters = {
        {"type", "object"},
        {"properties", {{"value", {{"type", "string"}}}}},
    };
    tool.execute = [calls, captured_arguments](
                       const std::string& arguments,
                       const acecode::ToolContext&) {
        if (calls) calls->fetch_add(1);
        if (captured_arguments) *captured_arguments = arguments;
        return acecode::ToolResult{"ok", true};
    };
    return tool;
}

std::vector<std::string> definition_names(
    const std::vector<acecode::ToolDef>& definitions) {
    std::vector<std::string> result;
    for (const auto& definition : definitions) {
        result.push_back(definition.name);
    }
    return result;
}

} // namespace

TEST(ToolProtocolNames, DeclaresOnlyVerifiedOpenCodeMappings) {
    std::string error;
    ASSERT_TRUE(acecode::validate_model_tool_name_mappings(&error)) << error;

    std::map<std::string, std::string> actual;
    for (const auto& mapping : acecode::model_tool_name_mappings()) {
        actual.emplace(std::string(mapping.native_name),
                       std::string(mapping.public_name));
    }
    EXPECT_EQ(actual, (std::map<std::string, std::string>{
                          {"TodoWrite", "todowrite"},
                          {"file_edit", "edit"},
                          {"file_read", "read"},
                          {"file_write", "write"},
                      }));
    EXPECT_EQ(acecode::model_tool_name_for_native("bash"), "bash");
    EXPECT_FALSE(
        acecode::native_tool_name_for_public_alias("apply_patch").has_value());
}

TEST(ToolProtocolNames, TranslatesDefinitionsWithoutChangingSchemas) {
    std::vector<acecode::ToolDef> native = {
        {"file_read", "read description", {{"type", "object"}}},
        {"file_write", "write description", {{"required", {"value"}}}},
        {"file_edit", "edit description", {{"additionalProperties", false}}},
        {"TodoWrite", "todo description", {{"type", "array"}}},
        {"bash", "bash description", {{"type", "string"}}},
    };
    std::vector<acecode::ToolDef> model;
    std::string error;

    ASSERT_TRUE(acecode::translate_tool_definitions_for_model(
        native, model, &error)) << error;
    EXPECT_EQ(definition_names(model),
              (std::vector<std::string>{
                  "read", "write", "edit", "todowrite", "bash"}));
    ASSERT_EQ(model.size(), native.size());
    for (std::size_t i = 0; i < model.size(); ++i) {
        EXPECT_EQ(model[i].description, native[i].description);
        EXPECT_EQ(model[i].parameters, native[i].parameters);
    }
}

TEST(ToolProtocolNames, RejectsDuplicateOutboundPublicNames) {
    const std::vector<acecode::ToolDef> native = {
        {"file_write", "mapped", nlohmann::json::object()},
        {"write", "native collision", nlohmann::json::object()},
    };
    std::vector<acecode::ToolDef> model = {
        {"sentinel", "unchanged on failure", nlohmann::json::object()},
    };
    std::string error;

    EXPECT_FALSE(acecode::translate_tool_definitions_for_model(
        native, model, &error));
    EXPECT_NE(error.find("duplicate model-facing tool name 'write'"),
              std::string::npos);
    ASSERT_EQ(model.size(), 1u);
    EXPECT_EQ(model.front().name, "sentinel");
}

TEST(ToolProtocolNames, RewritesProviderHistoryNameOnly) {
    acecode::ChatMessage assistant;
    assistant.role = "assistant";
    assistant.tool_calls = nlohmann::json::array({
        {
            {"id", "call-17"},
            {"type", "function"},
            {"function", {
                {"name", "file_write"},
                {"arguments", R"({"value":"unchanged"})"},
            }},
        },
    });
    const nlohmann::json original = assistant.tool_calls;
    acecode::ChatMessage result = acecode::ToolExecutor::format_tool_result(
        "call-17", acecode::ToolResult{"ok", true});
    std::vector<acecode::ChatMessage> messages = {assistant, result};

    acecode::rewrite_tool_calls_for_model(messages);

    ASSERT_EQ(messages[0].tool_calls.size(), 1u);
    EXPECT_EQ(messages[0].tool_calls[0]["function"]["name"], "write");
    EXPECT_EQ(messages[0].tool_calls[0]["id"], original[0]["id"]);
    EXPECT_EQ(messages[0].tool_calls[0]["function"]["arguments"],
              original[0]["function"]["arguments"]);
    EXPECT_EQ(messages[1].tool_call_id, "call-17");
    EXPECT_EQ(messages[1].content, "ok");
}

TEST(ToolProtocolNames, ResolvesPublicAndNativeNamesToRegisteredHandler) {
    std::atomic<int> calls{0};
    std::string captured_arguments;
    acecode::ToolExecutor tools;
    ASSERT_TRUE(tools.register_tool(make_protocol_tool(
        "file_write", &calls, &captured_arguments)));

    const auto definitions = tools.get_model_tool_definitions();
    ASSERT_EQ(definitions.size(), 1u);
    EXPECT_EQ(definitions.front().name, "write");
    EXPECT_EQ(definitions.front().description, "protocol test tool");

    EXPECT_EQ(tools.resolve_model_tool_name_to_native("write"), "file_write");
    EXPECT_EQ(tools.resolve_model_tool_name_to_native("file_write"),
              "file_write");
    EXPECT_EQ(tools.resolve_model_tool_name_to_native("todowrite"),
              "todowrite");

    const std::string arguments = R"({"value":"payload"})";
    const auto result = tools.execute(
        tools.resolve_model_tool_name_to_native("write"), arguments);
    EXPECT_TRUE(result.success);
    EXPECT_EQ(calls.load(), 1);
    EXPECT_EQ(captured_arguments, arguments);
}

TEST(ToolProtocolNames, RejectsPublicCollisionsInEitherRegistrationOrder) {
    acecode::ToolExecutor mapped_first;
    ASSERT_TRUE(mapped_first.register_tool(make_protocol_tool("file_write")));
    EXPECT_FALSE(mapped_first.register_tool(make_protocol_tool("write")));
    EXPECT_TRUE(mapped_first.has_tool("file_write"));
    EXPECT_FALSE(mapped_first.has_tool("write"));

    acecode::ToolExecutor native_first;
    ASSERT_TRUE(native_first.register_tool(make_protocol_tool("write")));
    EXPECT_FALSE(native_first.register_tool(make_protocol_tool("file_write")));
    EXPECT_TRUE(native_first.has_tool("write"));
    EXPECT_FALSE(native_first.has_tool("file_write"));
    EXPECT_EQ(native_first.resolve_model_tool_name_to_native("write"), "write");
}
