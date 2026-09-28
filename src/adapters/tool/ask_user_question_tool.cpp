#include "ask_user_question_tool.hpp"

#include "ask_user_question_types.hpp"

#include "permissions/interaction_mode.hpp"
#include "session/session_manager.hpp"
#include "utils/logger.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <sstream>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cctype>
#include <set>
#include <string>

namespace acecode {

namespace {

// 数一个 UTF-8 字符串的 codepoint 数 —— 12 字符上限必须按字符而不是字节算,
// 不然 "授权方式" (12 bytes in UTF-8, 4 chars) 会被误判越界。
std::size_t utf8_codepoint_count(const std::string& s) {
    std::size_t count = 0;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(s.data());
    std::size_t len = s.size();
    for (std::size_t i = 0; i < len;) {
        unsigned char c = p[i];
        int seq = 1;
        if ((c & 0x80) == 0x00) seq = 1;
        else if ((c & 0xE0) == 0xC0) seq = 2;
        else if ((c & 0xF0) == 0xE0) seq = 3;
        else if ((c & 0xF8) == 0xF0) seq = 4;
        else { i++; continue; }
        i += seq;
        count++;
    }
    return count;
}

constexpr int kMaxHeaderChars = 12;
constexpr int kMinOptions = 2;
// kDefaultAskMaxOptions / kMinAskMaxOptions / kMaxAskMaxOptions 见头文件。

bool label_has_recommended_suffix(const std::string& label) {
    return ask_option_label_has_recommended_suffix(label);
}

std::string timeout_fallback_answer(const AskQuestion& question) {
    for (const auto& option : question.options) {
        if (option.recommended) return option.label;
    }
    return "Not answered";
}

// 工具 description —— 对齐 claudecodehaha `ASK_USER_QUESTION_TOOL_PROMPT`
// 原文,删除 ACECode 没有对应概念的 `Plan mode note:` 段。
constexpr const char* kToolDescription =
    "Asks the user multiple choice questions to gather information, clarify "
    "ambiguity, understand preferences, make decisions or offer them choices. "
    "Use this tool when you need to ask the user questions during execution. "
    "This allows you to:\n"
    "1. Gather user preferences or requirements\n"
    "2. Clarify ambiguous instructions\n"
    "3. Get decisions on implementation choices as you work\n"
    "4. Offer choices to the user about what direction to take.\n"
    "\n"
    "Usage notes:\n"
    "- Users will always be able to select \"Other\" to provide custom text input\n"
    "- Use multiSelect: true to allow multiple answers to be selected for a question\n"
    "- If you recommend a specific option, make that the first option in the list "
    "and add \"(Recommended)\" at the end of the label";

} // namespace

std::optional<std::vector<AskQuestion>> validate_ask_user_question_args(
    const std::string& arguments_json, std::string& err) {
    return validate_ask_user_question_args(
        arguments_json, err, kDefaultAskMaxQuestions);
}

std::optional<std::vector<AskQuestion>> validate_ask_user_question_args(
    const std::string& arguments_json, std::string& err, int max_questions) {
    return validate_ask_user_question_args(
        arguments_json, err, max_questions, kDefaultAskMaxOptions);
}

std::optional<std::vector<AskQuestion>> validate_ask_user_question_args(
    const std::string& arguments_json, std::string& err, int max_questions,
    int max_options) {
    err.clear();
    if (arguments_json.empty()) {
        err = "[Error] AskUserQuestion requires arguments.";
        return std::nullopt;
    }

    nlohmann::json root;
    try {
        root = nlohmann::json::parse(arguments_json);
    } catch (const std::exception& e) {
        err = std::string("[Error] Failed to parse arguments JSON: ") + e.what();
        return std::nullopt;
    }

    const int effective_max_questions = std::clamp(
        max_questions, kMinAskQuestions, kMaxAskQuestions);
    const int effective_max_options = std::clamp(
        max_options, kMinAskMaxOptions, kMaxAskMaxOptions);
    if (!root.is_object() || !root.contains("questions") || !root["questions"].is_array()) {
        err = "[Error] `questions` must be an array (length 1-" +
              std::to_string(effective_max_questions) + ").";
        return std::nullopt;
    }
    const auto& qs = root["questions"];
    if (qs.size() < kMinAskQuestions ||
        qs.size() > static_cast<std::size_t>(effective_max_questions)) {
        err = "[Error] `questions` length must be between 1 and " +
              std::to_string(effective_max_questions) + " (got " +
              std::to_string(qs.size()) +
              "). Split the questions across multiple AskUserQuestion calls.";
        return std::nullopt;
    }

    std::vector<AskQuestion> out;
    out.reserve(qs.size());
    std::set<std::string> seen_questions;

    for (std::size_t qi = 0; qi < qs.size(); ++qi) {
        const auto& q = qs[qi];
        if (!q.is_object()) {
            err = "[Error] questions[" + std::to_string(qi) + "] must be an object.";
            return std::nullopt;
        }
        AskQuestion parsed;
        parsed.question = q.value("question", std::string{});
        parsed.header = q.value("header", std::string{});
        parsed.multi_select = q.value("multiSelect", false);

        if (parsed.question.empty()) {
            err = "[Error] questions[" + std::to_string(qi) +
                  "].question must be a non-empty string.";
            return std::nullopt;
        }
        if (parsed.header.empty()) {
            err = "[Error] questions[" + std::to_string(qi) +
                  "].header must be a non-empty string.";
            return std::nullopt;
        }
        if (utf8_codepoint_count(parsed.header) >
            static_cast<std::size_t>(kMaxHeaderChars)) {
            err = "[Error] questions[" + std::to_string(qi) +
                  "].header is too long (max 12 characters).";
            return std::nullopt;
        }

        if (!seen_questions.insert(parsed.question).second) {
            err = "[Error] Question texts must be unique across `questions`.";
            return std::nullopt;
        }

        if (!q.contains("options") || !q["options"].is_array()) {
            err = "[Error] questions[" + std::to_string(qi) +
                  "].options must be an array (length 2-" +
                  std::to_string(effective_max_options) + ").";
            return std::nullopt;
        }
        const auto& opts = q["options"];
        if (opts.size() < kMinOptions ||
            opts.size() > static_cast<std::size_t>(effective_max_options)) {
            err = "[Error] questions[" + std::to_string(qi) +
                  "].options length must be between 2 and " +
                  std::to_string(effective_max_options) + " (got " +
                  std::to_string(opts.size()) + ").";
            return std::nullopt;
        }

        std::set<std::string> seen_labels;
        for (std::size_t oi = 0; oi < opts.size(); ++oi) {
            const auto& o = opts[oi];
            if (!o.is_object()) {
                err = "[Error] questions[" + std::to_string(qi) + "].options[" +
                      std::to_string(oi) + "] must be an object.";
                return std::nullopt;
            }
            AskOption opt;
            opt.label = o.value("label", std::string{});
            opt.description = o.value("description", std::string{});
            opt.recommended = label_has_recommended_suffix(opt.label);
            if (opt.label.empty()) {
                err = "[Error] questions[" + std::to_string(qi) + "].options[" +
                      std::to_string(oi) + "].label must be non-empty.";
                return std::nullopt;
            }
            if (!seen_labels.insert(opt.label).second) {
                err = "[Error] Option labels must be unique within questions[" +
                      std::to_string(qi) + "].";
                return std::nullopt;
            }
            // preview 字段如果存在,必须是字符串(类型错误早发现),但内容被忽略。
            if (o.contains("preview") && !o["preview"].is_null() && !o["preview"].is_string()) {
                err = "[Error] questions[" + std::to_string(qi) + "].options[" +
                      std::to_string(oi) + "].preview must be a string if present.";
                return std::nullopt;
            }
            parsed.options.push_back(std::move(opt));
        }

        out.push_back(std::move(parsed));
    }

    return out;
}

std::string format_ask_answers(
    const std::vector<std::string>& question_order,
    const std::map<std::string, std::string>& answers) {
    std::string out = "User has answered your questions: ";
    bool first = true;
    for (const auto& q : question_order) {
        auto it = answers.find(q);
        const std::string& a = (it == answers.end()) ? std::string{} : it->second;
        if (!first) out += ", ";
        out += "\"";
        out += q;
        out += "\"=\"";
        out += a;
        out += "\"";
        first = false;
    }
    return out;
}

nlohmann::json build_ask_user_question_result_metadata(
    const std::vector<std::string>& question_order,
    const std::map<std::string, std::string>& answers,
    const std::set<std::string>* auto_selected_questions,
    const std::set<std::string>* multi_select_questions) {
    nlohmann::json items = nlohmann::json::array();
    for (const auto& q : question_order) {
        auto it = answers.find(q);
        const std::string& a = (it == answers.end()) ? std::string{} : it->second;
        const bool auto_selected = auto_selected_questions != nullptr &&
            auto_selected_questions->count(q) != 0;
        // multi_select 与 auto_selected 一样属于 UI 展示所需的形状信息:反馈卡
        // 会在落盘消息上重建,不带上它就只能在重载后丢掉「(多选)」标注。
        const bool multi_select = multi_select_questions != nullptr &&
            multi_select_questions->count(q) != 0;
        items.push_back({
            {"question", q},
            {"answer", a},
            {"auto_selected", auto_selected},
            {"multi_select", multi_select},
        });
    }
    return nlohmann::json{
        {"ask_user_question_result", {
            {"items", std::move(items)}
        }}
    };
}

std::string format_ask_user_question_result_display(
    const nlohmann::json& metadata) {
    if (!metadata.is_object()) return {};

    auto result_it = metadata.find("ask_user_question_result");
    if (result_it == metadata.end() || !result_it->is_object()) return {};

    auto items_it = result_it->find("items");
    if (items_it == result_it->end() || !items_it->is_array()) return {};

    std::vector<std::pair<std::string, std::string>> items;
    std::vector<bool> auto_selected;
    items.reserve(items_it->size());
    auto_selected.reserve(items_it->size());
    for (const auto& item : *items_it) {
        if (!item.is_object()) continue;
        auto q_it = item.find("question");
        auto a_it = item.find("answer");
        if (q_it == item.end() || a_it == item.end() ||
            !q_it->is_string() || !a_it->is_string()) {
            continue;
        }
        items.emplace_back(q_it->get<std::string>(),
                           a_it->get<std::string>());
        auto_selected.push_back(item.value("auto_selected", false));
    }

    if (items.empty()) return {};

    // One `question：answer` pair per item, blank line between items. The
    // question is always part of the row: a transcript that only lists answers
    // is unreadable once the surrounding chat has scrolled away.
    const std::string separator = "\xEF\xBC\x9A";  // fullwidth colon
    std::ostringstream out;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i > 0) out << "\n\n";
        out << (i + 1) << ". " << items[i].first << separator;
        if (i < auto_selected.size() && auto_selected[i]) {
            out << "[Auto-selected] ";
        }
        out << items[i].second;
    }
    return out.str();
}

ToolResult make_rejected_ask_result() {
    ToolResult r;
    r.output = "[Error] User declined to answer questions.";
    r.success = false;
    r.metadata = {{"ask_user_question_result", {
        {"cancelled", true}, {"items", nlohmann::json::array()}
    }}};
    return r;
}

ToolResult make_interjected_ask_result() {
    ToolResult r;
    r.success = true;
    r.output =
        "[User interjected] The user did not answer these questions. Instead "
        "they sent a new message while the questions were pending; it is the "
        "next real user message after this tool result. Treat that message as "
        "the user's actual instruction and continue from it. Do not ask these "
        "questions again unless that message leaves them genuinely unresolved.";
    r.metadata = {{"ask_user_question_result", {
        {"interjected", true}, {"items", nlohmann::json::array()}
    }}};
    return r;
}

// Headless(-p / --print)模式的自动应答。success=true 防止模型当失败重问。
ToolResult make_headless_ask_result() {
    ToolResult r;
    r.success = true;
    r.output =
        "[Headless mode] The user cannot answer questions in print (-p) mode. "
        "Do not wait and do not ask again. Decide autonomously: pick the "
        "recommended option if one exists, otherwise the most reasonable "
        "option, note the decision briefly in your response, and continue.";
    return r;
}

ToolResult make_policy_denied_ask_result(const char* origin) {
    ToolResult r;
    r.success = true;
    r.output =
        "[Question policy: deny] Interactive questions are disabled for this "
        "session. Do not wait and do not ask again. Decide autonomously: pick "
        "the recommended option if one exists, otherwise the most reasonable "
        "assumption, note the decision briefly in your response, and continue "
        "with the task.";
    r.metadata = nlohmann::json{
        {"ask_user_question_auto", {
            {"mode", "deny"},
            {"origin", origin ? origin : "explicit"},
        }}
    };
    return r;
}

// 收集多选题目文本,供 build_ask_user_question_result_metadata 在落盘元数据里
// 标注 (多选),使反馈卡在重载后仍能还原题型。
static std::set<std::string> multi_select_question_set(
    const std::vector<AskQuestion>& questions) {
    std::set<std::string> out;
    for (const auto& q : questions) {
        if (q.multi_select) out.insert(q.question);
    }
    return out;
}

ToolResult make_timeout_adopted_ask_result(
    const std::vector<AskQuestion>& questions,
    const std::vector<std::string>& question_order,
    int timeout_seconds,
    const std::map<std::string, std::string>* adopted_answers,
    const std::set<std::string>* adopted_auto_selected_questions) {
    std::map<std::string, std::string> answers;
    if (adopted_answers) {
        answers = *adopted_answers;
    }
    std::set<std::string> auto_selected_questions;
    if (adopted_auto_selected_questions) {
        auto_selected_questions = *adopted_auto_selected_questions;
    }
    for (const auto& q : questions) {
        // The presence of a key means the prompt channel returned an explicit
        // answer state. An empty value is intentional for an activated but
        // empty custom row (Not answered), so it must not trigger timeout
        // fallback. Only a missing question may adopt Recommended.
        const auto it = answers.find(q.question);
        if (it != answers.end()) continue;
        const std::string fallback = timeout_fallback_answer(q);
        answers[q.question] = fallback;
        if (fallback != "Not answered") {
            auto_selected_questions.insert(q.question);
        }
    }
    const std::set<std::string> multi_select_questions =
        multi_select_question_set(questions);
    ToolResult r;
    r.success = true;
    r.output =
        "[Question policy: timeout] The user did not answer within " +
        std::to_string(timeout_seconds) +
        " seconds. Existing answers were preserved; unanswered questions use "
        "their first Recommended option when available, otherwise Not answered. " +
        format_ask_answers(question_order, answers);
    r.metadata = build_ask_user_question_result_metadata(
        question_order, answers, &auto_selected_questions, &multi_select_questions);
    r.metadata["ask_user_question_auto"] = {
        {"mode", "timeout"},
        {"seconds", timeout_seconds},
    };
    return r;
}

static bool goal_unattended(const ToolContext& ctx) {
    return ctx.goal_unattended_active && ctx.goal_unattended_active();
}

// 解析生效策略:探针未注入(独立 ToolExecutor 调用)= Ask 维持旧行为。
static ResolvedQuestionPolicy effective_question_policy(const ToolContext& ctx) {
    if (goal_unattended(ctx)) {
        ResolvedQuestionPolicy policy;
        policy.policy = QuestionPolicy::Timeout;
        policy.timeout_seconds = kGoalQuestionTimeoutSeconds;
        policy.origin = "goal";
        return policy;
    }
    if (ctx.question_policy) return ctx.question_policy();
    return ResolvedQuestionPolicy{};
}

// TUI 专用的工厂已删除。工具逻辑本来就只有一份(参数校验、策略
// 判定、结果拼装全是下面那些共享函数),两端不同的只是「怎么把问题送
// 到人面前」。那段 TUI overlay 传输现在住在 src/tui/tui_ask_channel.cpp,
// 经 `ToolContext::ask_user_questions` 注入 —— 与 daemon 同一个口子。
// 副作用:本 TU 不再引用 ftxui。

namespace {

// 构造 daemon 工厂会用到的同一份 ToolDef。复用 create_ask_user_question_tool
// 那段拼装太长 —— 把 def 抽出来共享。
ToolDef build_ask_user_question_def(int max_questions, int max_options) {
    const int effective_max_questions = std::clamp(
        max_questions, kMinAskQuestions, kMaxAskQuestions);
    const int effective_max_options = std::clamp(
        max_options, kMinAskMaxOptions, kMaxAskMaxOptions);
    ToolDef def;
    def.name = "AskUserQuestion";
    def.description = kToolDescription;

    nlohmann::json option_schema = {
        {"type", "object"},
        {"required", nlohmann::json::array({"label", "description"})},
        {"properties", {
            {"label", {
                {"type", "string"},
                {"description",
                 "Short (1-5 word) label shown to the user as the selectable choice."}
            }},
            {"description", {
                {"type", "string"},
                {"description",
                 "Explanation of what this option means or what will happen if chosen."}
            }},
            {"preview", {
                {"type", "string"},
                {"description",
                 "Optional preview content. Accepted for SDK-schema parity."}
            }}
        }}
    };

    nlohmann::json question_schema = {
        {"type", "object"},
        {"required", nlohmann::json::array({"question", "header", "options"})},
        {"properties", {
            {"question", {
                {"type", "string"},
                {"description",
                 "The complete question. Should be clear, specific and end with '?'."}
            }},
            {"header", {
                {"type", "string"},
                {"description",
                 "Very short chip label (max 12 characters)."}
            }},
            {"options", {
                {"type", "array"},
                {"minItems", kMinOptions},
                {"maxItems", effective_max_options},
                {"items", option_schema},
                {"description",
                 "2-" + std::to_string(effective_max_options) +
                 " mutually exclusive choices. Do NOT include an 'Other' option — "
                 "the UI appends one automatically."}
            }},
            {"multiSelect", {
                {"type", "boolean"},
                {"default", false},
                {"description", "Set true to allow the user to pick multiple options."}
            }}
        }}
    };

    def.parameters = {
        {"type", "object"},
        {"required", nlohmann::json::array({"questions"})},
        {"properties", {
            {"questions", {
                {"type", "array"},
                {"minItems", kMinAskQuestions},
                {"maxItems", effective_max_questions},
                {"items", question_schema},
                {"description",
                 "1-" + std::to_string(effective_max_questions) +
                 " questions to ask the user. Question texts must be unique."}
            }}
        }}
    };
    return def;
}

// 把已 validate 的 question 列表转成 prompter 用的 questions_payload(给前端渲染)。
// 字段名与 design.md / spec.md 的 WS 协议对齐:每个 question 携带 id(用 question
// 文本作为 id,与 TUI 行为同步) / text / options[{label,value}] / multiSelect。
nlohmann::json questions_to_payload(const std::vector<AskQuestion>& qs) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& q : qs) {
        nlohmann::json options = nlohmann::json::array();
        for (const auto& o : q.options) {
            options.push_back({
                {"label", o.label},
                {"value", o.label}, // value=label,前端 v1 不区分两者
                {"description", o.description},
            });
        }
        arr.push_back({
            {"id",          q.question},
            {"text",        q.question},
            {"header",      q.header},
            {"options",     options},
            {"multiSelect", q.multi_select},
        });
    }
    return arr;
}

// 把 ctx.ask_user_questions 回来的 JSON 转成 std::map<question, answer_text>,
// 按 ", " 拼合 multiSelect。供 format_ask_answers 使用。
std::map<std::string, std::string>
parse_async_response(const nlohmann::json& resp_json,
                     std::set<std::string>* auto_selected_questions = nullptr) {
    std::map<std::string, std::string> answers;
    if (!resp_json.is_object()) return answers;
    if (!resp_json.contains("answers") || !resp_json["answers"].is_array()) return answers;
    for (const auto& a : resp_json["answers"]) {
        if (!a.is_object()) continue;
        std::string qid = a.value("question_id", std::string{});
        if (qid.empty()) continue;

        std::vector<std::string> parts;
        const bool not_answered = a.value("not_answered", false);
        const bool auto_selected = a.value("auto_selected", false);
        if (a.contains("selected") && a["selected"].is_array()) {
            for (const auto& s : a["selected"]) {
                if (s.is_string()) parts.push_back(s.get<std::string>());
            }
        }
        if (a.contains("custom_text") && a["custom_text"].is_string()) {
            std::string ct = a["custom_text"].get<std::string>();
            if (!ct.empty()) parts.push_back(ct);
        }

        std::string joined;
        for (std::size_t i = 0; i < parts.size(); ++i) {
            if (i) joined += ", ";
            joined += parts[i];
        }
        if (not_answered && joined.empty()) joined = "Not answered";
        if (auto_selected && auto_selected_questions) {
            auto_selected_questions->insert(qid);
        }
        answers[qid] = joined;
    }
    return answers;
}

} // namespace

ToolImpl create_ask_user_question_tool_async() {
    return create_ask_user_question_tool_async(
        kDefaultAskMaxQuestions, kDefaultAskMaxOptions);
}

ToolImpl create_ask_user_question_tool_async(int max_questions) {
    return create_ask_user_question_tool_async(
        max_questions, kDefaultAskMaxOptions);
}

ToolImpl create_ask_user_question_tool_async(int max_questions, int max_options) {
    const int effective_max_questions = std::clamp(
        max_questions, kMinAskQuestions, kMaxAskQuestions);
    const int effective_max_options = std::clamp(
        max_options, kMinAskMaxOptions, kMaxAskMaxOptions);
    auto execute = [effective_max_questions, effective_max_options](
                       const std::string& arguments_json,
                       const ToolContext& ctx) -> ToolResult {
        std::string err;
        auto parsed = validate_ask_user_question_args(
            arguments_json, err, effective_max_questions,
            effective_max_options);
        if (!parsed.has_value()) {
            return ToolResult{err, false};
        }

        // Headless(-p)进程:没有任何交互通道,自动应答(先于 goal 分支,
        // 两者同时成立时文案取 headless —— 对模型的环境解释更准确)。
        if (headless::active()) {
            LOG_INFO("[AskUserQuestion] auto-answered (headless print mode)");
            return make_headless_ask_result();
        }

        // 应答策略(add-ask-question-policy):deny 不发 question_request,
        // 直接自动应答。timeout 的等待窗口由 AskUserQuestionPrompter 持有
        // (session_registry 创建时注入),这里只消费响应里的 timed_out 标记。
        const ResolvedQuestionPolicy policy = effective_question_policy(ctx);
        if (policy.policy == QuestionPolicy::Deny) {
            LOG_INFO(std::string("[AskUserQuestion] auto-answered (deny policy, ") +
                     policy.origin + ")");
            return make_policy_denied_ask_result(policy.origin);
        }

        // ctx.ask_user_questions 为空 → daemon 没装 prompter,工具不可用。
        // 直接拒绝 + 让 LLM 知道(避免无限挂起)。
        if (!ctx.ask_user_questions) {
            return ToolResult{
                "[Error] AskUserQuestion is not supported by this session "
                "(no UI channel connected).",
                false};
        }

        // abort 已触发 = 不发问,直接 reject
        if (ctx.abort_flag && ctx.abort_flag->load()) {
            return make_rejected_ask_result();
        }

        std::vector<std::string> question_order;
        question_order.reserve(parsed->size());
        for (const auto& q : *parsed) question_order.push_back(q.question);

        nlohmann::json payload = questions_to_payload(*parsed);
        nlohmann::json resp = ctx.ask_user_questions(payload);

        // timeout 策略到期:prompter 已发 question_closed(reason=timeout)
        // 收掉前端 modal,这里合成自动采纳结果。TUI 会话已经按逐题规则
        // 收卷，优先采用它的结构化答案；只有没有答案通道时才回退到
        // 每题第一个选项。
        if (resp.value("timed_out", false)) {
            LOG_INFO("[AskUserQuestion] timeout policy adopted first options after " +
                     std::to_string(policy.timeout_seconds) + "s");
            std::set<std::string> adopted_auto_selected_questions;
            const auto adopted_answers = parse_async_response(
                resp, &adopted_auto_selected_questions);
            return make_timeout_adopted_ask_result(
                *parsed, question_order, policy.timeout_seconds,
                &adopted_answers, &adopted_auto_selected_questions);
        }

        // 插话先于 cancelled 判:两位同时为 true(见 AskUserQuestionResponse),
        // 这里要给模型的是「看下一条 user 消息」,不是 declined 错误。
        if (resp.value("interjected", false)) {
            LOG_INFO("[AskUserQuestion] resolved by user interjection");
            return make_interjected_ask_result();
        }

        bool cancelled = resp.value("cancelled", false);
        if (cancelled) {
            return make_rejected_ask_result();
        }

        auto answers = parse_async_response(resp);
        const std::set<std::string> multi_select_questions =
            multi_select_question_set(*parsed);
        ToolResult r;
        r.success = true;
        r.output  = format_ask_answers(question_order, answers);
        r.metadata = build_ask_user_question_result_metadata(
            question_order, answers, nullptr, &multi_select_questions);
        return r;
    };

    ToolImpl impl;
    impl.definition   = build_ask_user_question_def(
        effective_max_questions, effective_max_options);
    impl.execute      = execute;
    impl.is_read_only = true;
    impl.source       = ToolSource::Builtin;
    return impl;
}

} // namespace acecode
