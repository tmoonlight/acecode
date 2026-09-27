#include <gtest/gtest.h>

#include "provider/llm_provider.hpp"
#include "tool/builtin_tool_registry.hpp"
#include "tool/tool_executor.hpp"
#include "tool/tool_protocol_names.hpp"
#include "utils/tool_errors.hpp"

#include <atomic>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {

acecode::ToolImpl make_protocol_tool(
    std::string name,
    std::atomic<int>* calls = nullptr,
    std::string* captured_arguments = nullptr,
    std::string description = "protocol test tool") {
    acecode::ToolImpl tool;
    tool.definition.name = std::move(name);
    tool.definition.description = std::move(description);
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

// 整词包含:前后都不是 [A-Za-z0-9_]。
bool contains_word(const std::string& text, const std::string& word) {
    auto is_name_char = [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
               (c >= '0' && c <= '9') || c == '_';
    };
    std::size_t pos = 0;
    while ((pos = text.find(word, pos)) != std::string::npos) {
        const bool before_ok = pos == 0 || !is_name_char(text[pos - 1]);
        const std::size_t end = pos + word.size();
        const bool after_ok = end >= text.size() || !is_name_char(text[end]);
        if (before_ok && after_ok) return true;
        pos = end;
    }
    return false;
}

} // namespace

// 场景:进程刚启动、没有加载任何「工具重写」文件。
// 期望:生效映射为空,所有名字原样透传 —— 模型看到的就是原生名。
// 回归:改动前四条 OpenCode 别名是编译期常量、永远生效,与设置页的
// opt-in 语义冲突。
TEST(ToolProtocolNames, NoMappingsByDefaultKeepsNativeNames) {
    acecode::ScopedModelToolNameMappings scoped({});
    EXPECT_TRUE(acecode::model_tool_name_mappings().empty());
    EXPECT_EQ(acecode::model_tool_name_for_native("file_read"), "file_read");
    EXPECT_FALSE(acecode::native_tool_name_for_public_alias("read").has_value());
    EXPECT_EQ(acecode::rewrite_model_facing_text("call file_read"), "call file_read");
}

// 场景:内置种子(用于 tool-rewrites.json 首次生成)必须恰好是四条 OpenCode 名。
// 期望:种子内容固定;未映射工具(bash)与未知别名(apply_patch)不受影响。
TEST(ToolProtocolNames, SeedDeclaresOnlyVerifiedOpenCodeMappings) {
    std::string error;
    ASSERT_TRUE(acecode::validate_model_tool_name_mappings(
        acecode::default_model_tool_name_mappings(), &error)) << error;

    std::map<std::string, std::string> actual;
    for (const auto& mapping : acecode::default_model_tool_name_mappings()) {
        actual.emplace(mapping.native_name, mapping.public_name);
    }
    EXPECT_EQ(actual, (std::map<std::string, std::string>{
                          {"TodoWrite", "todowrite"},
                          {"file_edit", "edit"},
                          {"file_read", "read"},
                          {"file_write", "write"},
                      }));

    acecode::ScopedModelToolNameMappings scoped(
        acecode::default_model_tool_name_mappings());
    EXPECT_EQ(acecode::model_tool_name_for_native("bash"), "bash");
    EXPECT_EQ(acecode::model_tool_name_for_native("file_read"), "read");
    EXPECT_FALSE(
        acecode::native_tool_name_for_public_alias("apply_patch").has_value());
}

// 场景:发布一组非法映射(public 名撞另一条的 native 名 / 含非法字符 / 重复)。
// 期望:set 返回 false 且原映射保持不变;RAII 析构后恢复上一份映射。
TEST(ToolProtocolNames, RejectsInvalidMappingsAndRestoresOnScopeExit) {
    acecode::ScopedModelToolNameMappings outer(
        acecode::ToolProtocolNameMappings{{"file_read", "read"}});
    {
        std::string error;
        EXPECT_FALSE(acecode::set_model_tool_name_mappings(
            {{"file_read", "file_write"}, {"file_write", "write"}}, &error));
        EXPECT_NE(error.find("collides with a mapped native name"), std::string::npos);
        EXPECT_FALSE(acecode::set_model_tool_name_mappings(
            {{"file_read", "re ad"}}, &error));
        EXPECT_NE(error.find("must match"), std::string::npos);
        EXPECT_FALSE(acecode::set_model_tool_name_mappings(
            {{"file_read", "x"}, {"file_edit", "x"}}, &error));
        EXPECT_NE(error.find("duplicate public"), std::string::npos);
        EXPECT_FALSE(acecode::set_model_tool_name_mappings(
            {{"file_read", "file_read"}}, &error));
        EXPECT_NE(error.find("must change"), std::string::npos);
        // 失败的发布不改动生效映射。
        EXPECT_EQ(acecode::model_tool_name_for_native("file_read"), "read");

        acecode::ScopedModelToolNameMappings inner(
            acecode::ToolProtocolNameMappings{{"bash", "shell"}});
        EXPECT_EQ(acecode::model_tool_name_for_native("bash"), "shell");
        EXPECT_EQ(acecode::model_tool_name_for_native("file_read"), "file_read");
    }
    EXPECT_EQ(acecode::model_tool_name_for_native("bash"), "bash");
    EXPECT_EQ(acecode::model_tool_name_for_native("file_read"), "read");
}

// 场景:provider 工具名合法性(OpenAI 与 Anthropic 交集 ^[A-Za-z0-9_-]{1,64}$)。
TEST(ToolProtocolNames, ValidatesProviderToolNameCharset) {
    EXPECT_TRUE(acecode::is_valid_model_tool_name("read"));
    EXPECT_TRUE(acecode::is_valid_model_tool_name("Todo-Write_2"));
    EXPECT_FALSE(acecode::is_valid_model_tool_name(""));
    EXPECT_FALSE(acecode::is_valid_model_tool_name("read file"));
    EXPECT_FALSE(acecode::is_valid_model_tool_name("read.file"));
    EXPECT_FALSE(acecode::is_valid_model_tool_name(u8"读文件"));
    EXPECT_FALSE(acecode::is_valid_model_tool_name(std::string(65, 'a')));
}

// 场景:给模型看的文案里整词出现原生名。
// 期望:只替换整词(反引号 / 空格 / 标点相邻都算),file_read_tool 这类
// 更长的标识符不动;单遍最长匹配,不会把替换结果再次替换。
TEST(ToolProtocolNames, RewritesWholeWordsOnlyWithoutChaining) {
    const acecode::ToolProtocolNameMappings mappings{
        {"file_read", "read"},
        {"file_read_extended", "read_ext"},
        {"TodoWrite", "todowrite"},
    };
    EXPECT_EQ(acecode::rewrite_model_facing_text(
                  "Use `file_read` first; file_read. Then TodoWrite(", mappings),
              "Use `read` first; read. Then todowrite(");
    EXPECT_EQ(acecode::rewrite_model_facing_text(
                  "file_read_tool my_file_read file_read_extended", mappings),
              "file_read_tool my_file_read read_ext");
    EXPECT_EQ(acecode::rewrite_model_facing_text("", mappings), "");
    EXPECT_EQ(acecode::rewrite_model_facing_text("file_read", {}), "file_read");
}

// 场景:翻译工具定义。
// 期望:名字改写、description 与参数 schema 里的 description 整词改写,
// 参数结构其余部分逐字节不变。
TEST(ToolProtocolNames, TranslatesDefinitionsAndDescriptionsOnly) {
    acecode::ScopedModelToolNameMappings scoped(
        acecode::default_model_tool_name_mappings());
    std::vector<acecode::ToolDef> native = {
        {"file_read", "Read a file. Call file_read again only if changed.",
         {{"type", "object"},
          {"properties", {{"file_path", {{"type", "string"},
                                          {"description", "Path passed to file_read"}}}}},
          {"required", {"file_path"}}}},
        {"file_write", "write description", {{"required", {"value"}}}},
        {"file_edit", "edit description", {{"additionalProperties", false}}},
        {"TodoWrite", "todo description", {{"type", "array"}}},
        {"bash", "bash description; prefer file_read for files", {{"type", "string"}}},
    };
    std::vector<acecode::ToolDef> model;
    std::string error;

    ASSERT_TRUE(acecode::translate_tool_definitions_for_model(
        native, model, &error)) << error;
    EXPECT_EQ(definition_names(model),
              (std::vector<std::string>{
                  "read", "write", "edit", "todowrite", "bash"}));
    ASSERT_EQ(model.size(), native.size());
    EXPECT_EQ(model[0].description, "Read a file. Call read again only if changed.");
    EXPECT_EQ(model[0].parameters["properties"]["file_path"]["description"],
              "Path passed to read");
    EXPECT_EQ(model[0].parameters["required"], native[0].parameters["required"]);
    EXPECT_EQ(model[0].parameters["type"], "object");
    EXPECT_EQ(model[4].description, "bash description; prefer read for files");
    for (std::size_t i = 1; i < 4; ++i) {
        EXPECT_EQ(model[i].description, native[i].description);
        EXPECT_EQ(model[i].parameters, native[i].parameters);
    }
}

// 场景:两个原生工具翻译后得到同一个模型侧名。
// 期望:翻译失败、输出参数不被改动。
TEST(ToolProtocolNames, RejectsDuplicateOutboundPublicNames) {
    acecode::ScopedModelToolNameMappings scoped(
        acecode::ToolProtocolNameMappings{{"file_write", "write"}});
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

// 场景:ToolExecutor 在映射为空时注册了 file_write 与 write 两个工具,之后
// 用户启用把 file_write 重写成 write 的映射(运行期冲突)。
// 期望:模型侧定义回退为原生定义而不是空表 —— 空表意味着模型零工具静默运行。
TEST(ToolProtocolNames, FallsBackToNativeDefinitionsWhenTranslationCollides) {
    acecode::ToolExecutor tools;
    {
        acecode::ScopedModelToolNameMappings none({});
        ASSERT_TRUE(tools.register_tool(make_protocol_tool("file_write")));
        ASSERT_TRUE(tools.register_tool(make_protocol_tool("write")));
    }
    acecode::ScopedModelToolNameMappings scoped(
        acecode::ToolProtocolNameMappings{{"file_write", "write"}});
    const auto definitions = tools.get_model_tool_definitions();
    EXPECT_EQ(definition_names(definitions),
              (std::vector<std::string>{"file_write", "write"}));
    EXPECT_EQ(tools.get_model_tool_definitions_by_source(acecode::ToolSource::Builtin).size(), 2u);
}

// 场景:历史里 assistant 的 tool_calls 用原生名。
// 期望:只改 function.name,id / arguments / 配对的 tool 结果原样。
TEST(ToolProtocolNames, RewritesProviderHistoryNameOnly) {
    acecode::ScopedModelToolNameMappings scoped(
        acecode::default_model_tool_name_mappings());
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

// 场景:模型用 public 名或原生名调用。
// 期望:两者都解析到同一个已注册 handler;未注册的别名原样返回。
TEST(ToolProtocolNames, ResolvesPublicAndNativeNamesToRegisteredHandler) {
    acecode::ScopedModelToolNameMappings scoped(
        acecode::default_model_tool_name_mappings());
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

// 场景:映射生效时,注册顺序不同的两种 public 名冲突。
// 期望:后注册者被拒,先注册者保留。
TEST(ToolProtocolNames, RejectsPublicCollisionsInEitherRegistrationOrder) {
    acecode::ScopedModelToolNameMappings scoped(
        acecode::default_model_tool_name_mappings());
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

// 场景:启用种子映射后取全部内置工具的模型侧定义。
// 期望:任何工具的 description(含参数 schema 里的 description)都不再整词
// 出现被映射的原生名 —— 工具表叫 read、描述里却写 file_read 就是复盘里
// 「模型看到两套名字」的根源。
TEST(ToolProtocolNames, BuiltinToolDescriptionsUseModelFacingNamesWhenMapped) {
    acecode::ScopedModelToolNameMappings scoped(
        acecode::default_model_tool_name_mappings());
    acecode::AppConfig config;
    config.web_search.enabled = false;
    acecode::ToolExecutor tools;
    acecode::register_session_builtin_tools(tools, config);

    const auto definitions = tools.get_model_tool_definitions();
    ASSERT_FALSE(definitions.empty());
    for (const auto& definition : definitions) {
        for (const auto& mapping : acecode::default_model_tool_name_mappings()) {
            EXPECT_FALSE(contains_word(definition.description, mapping.native_name))
                << definition.name << " description still mentions "
                << mapping.native_name;
            EXPECT_FALSE(contains_word(definition.parameters.dump(), mapping.native_name))
                << definition.name << " schema still mentions "
                << mapping.native_name;
        }
    }
}

// 场景:工具执行期生成的错误文案(不经过定义翻译)。
// 期望:文案里的工具名跟随生效映射;映射为空时保持原生名(老测试与老会话不变)。
TEST(ToolProtocolNames, ToolErrorTextsFollowActiveMapping) {
    {
        acecode::ScopedModelToolNameMappings none({});
        EXPECT_NE(acecode::ToolErrors::file_not_read_for_edit("a.txt").find(
                      "Read the target file with file_read before"),
                  std::string::npos);
    }
    acecode::ScopedModelToolNameMappings scoped(
        acecode::default_model_tool_name_mappings());
    EXPECT_NE(acecode::ToolErrors::file_not_read_for_edit("a.txt").find(
                  "Read the target file with read before"),
              std::string::npos);
    EXPECT_NE(acecode::ToolErrors::string_not_found("a.txt").find("with read and retry"),
              std::string::npos);
    EXPECT_NE(acecode::ToolErrors::legacy_range_edit_arguments("a.txt").find(
                  "[Error] edit no longer supports"),
              std::string::npos);
    EXPECT_NE(acecode::ToolErrors::notebook_edit_required("a.ipynb").find(
                  "instead of edit:"),
              std::string::npos);
}
