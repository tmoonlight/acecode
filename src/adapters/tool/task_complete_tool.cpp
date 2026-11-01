#include "task_complete_tool.hpp"
#include "llm/tool_icons.hpp"
#include "utils/text.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>

namespace acecode {

namespace {

// 长度与按钮上限照搬 Claude 桌面版 wrap_up:recap 约 300 字符、问题 100 字符、
// 按钮 1–2 个、按钮文字 24 字符 / 至多 4 个词。只有按钮文字按上限截断(按钮放不下),
// 其余超长只在回执里提醒模型下一次写短,不改动内容。
constexpr std::size_t kSummaryTargetChars = 300;
constexpr std::size_t kAskMaxChars = 100;
constexpr std::size_t kMaxActions = 2;
constexpr std::size_t kLabelMaxChars = 24;
constexpr std::size_t kLabelMaxWords = 4;

// 工具 description。措辞对齐 Claude 桌面版 wrap_up;差别只有调用时机:ACECode 的
// task_complete 是终止信号,结束语必须写在它之前(紧挨它的那条 assistant 消息
// 就是用户读到的完整回答)。
constexpr const char* kToolDescription =
    "Fill the \"next step\" box shown at the end of your turn, and end the turn: a "
    "two-or-three-sentence recap and, when this turn leaves the user a decision to "
    "make or a step to take, one question and buttons that answer it; a turn with "
    "nothing pending ends with the recap alone, and no box is drawn. Call it once in "
    "EVERY turn in which you changed files, ran commands, or completed multi-step "
    "work, as the very last thing you do: the agent loop returns control to the user "
    "as soon as it succeeds. Skip it for short answers, pure conversation, or a brief "
    "read-and-explain turn; a reply that names a follow-up is not brief: ask or "
    "propose it, and call this.\n"
    "\n"
    "Write your closing message before this call: ordinary text, your answer to the "
    "user in full, which the recap never replaces. When the box asks your question, "
    "do not ask it again in that message.\n"
    "\n"
    "Do NOT call this tool to check whether the user wants you to continue (just keep "
    "working), or to get a choice you need before you can go on with the current task "
    "(use AskUserQuestion for that).\n"
    "\n"
    "Fields: summary and ask are said to the user, in the second person and in the "
    "user's language; a reply button's label is the user's words back to you.\n"
    "- summary: what is now true or changed, then where it stands, including what is "
    "not done or not verified (for example: nothing committed, or not checked in the "
    "app). Two or three short sentences, about 40 words / 300 characters. No "
    "recommendation here, and never point at anything else in the conversation "
    "(\"listed below\", \"see above\").\n"
    "- ask (with actions, only when the next move is the user's call: a choice of "
    "approach, a go-ahead to fix, commit, push, post or send, an approval): just that "
    "ONE decision in one short sentence, never two joined by \"and\" (\"Which fix "
    "first?\"), with no reason or recommendation and without listing the options the "
    "buttons give: your pick, actions[0], is drawn as the primary button. At most 100 "
    "characters.\n"
    "Every field is a top-level parameter holding plain prose (actions: a JSON array); "
    "none contains markup or another field.";

// 解码一个 UTF-8 码点;非法字节按单字节返回 U+FFFD,保证前进。
std::uint32_t next_code_point(const std::string& text, std::size_t& i) {
    const auto byte = [&text](std::size_t at) {
        return static_cast<unsigned char>(text[at]);
    };
    const unsigned char lead = byte(i);
    std::size_t length = 1;
    std::uint32_t value = lead;
    if (lead >= 0xF0 && lead <= 0xF4) { length = 4; value = lead & 0x07; }
    else if (lead >= 0xE0 && lead <= 0xEF) { length = 3; value = lead & 0x0F; }
    else if (lead >= 0xC2 && lead <= 0xDF) { length = 2; value = lead & 0x1F; }
    else if (lead >= 0x80) { ++i; return 0xFFFD; }
    if (length > 1) {
        if (i + length > text.size()) { ++i; return 0xFFFD; }
        for (std::size_t k = 1; k < length; ++k) {
            const unsigned char continuation = byte(i + k);
            if ((continuation & 0xC0) != 0x80) { ++i; return 0xFFFD; }
            value = (value << 6) | (continuation & 0x3F);
        }
    }
    i += length;
    return value;
}

std::size_t code_point_count(const std::string& text) {
    std::size_t count = 0;
    for (std::size_t i = 0; i < text.size(); ++count) next_code_point(text, i);
    return count;
}

std::string code_point_prefix(const std::string& text, std::size_t limit) {
    std::size_t i = 0;
    for (std::size_t count = 0; i < text.size() && count < limit; ++count) {
        next_code_point(text, i);
    }
    return text.substr(0, i);
}

// 看不见的字符(Unicode Default_Ignorable 与控制字符的常见区段):按钮文字带着
// 它们会藏住真正发出去的内容,Claude 桌面版直接丢弃这样的按钮,这里同样处理。
bool is_invisible_code_point(std::uint32_t cp) {
    if (cp < 0x20 || (cp >= 0x7F && cp <= 0x9F)) return true;
    return cp == 0x00AD || cp == 0x034F || cp == 0x061C || cp == 0x115F ||
        cp == 0x1160 || cp == 0x17B4 || cp == 0x17B5 ||
        (cp >= 0x180B && cp <= 0x180F) || (cp >= 0x200B && cp <= 0x200F) ||
        (cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2060 && cp <= 0x206F) ||
        cp == 0x2800 || cp == 0x3164 || (cp >= 0xFE00 && cp <= 0xFE0F) ||
        cp == 0xFEFF || cp == 0xFFA0 || (cp >= 0xFFF0 && cp <= 0xFFF8) ||
        cp == 0xFFFC || (cp >= 0x1BCA0 && cp <= 0x1BCA3) ||
        (cp >= 0x1D173 && cp <= 0x1D17A) || (cp >= 0xE0000 && cp <= 0xE0FFF);
}

bool is_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

// 连续空白折成一个空格并去掉首尾空白(按钮文字只有一行)。
std::string single_line(const std::string& text) {
    std::string out;
    bool pending_space = false;
    for (unsigned char c : text) {
        if (is_space(c)) { pending_space = !out.empty(); continue; }
        if (pending_space) out.push_back(' ');
        pending_space = false;
        out.push_back(static_cast<char>(c));
    }
    return out;
}

std::size_t word_count(const std::string& line) {
    std::size_t words = line.empty() ? 0 : 1;
    for (char c : line) words += c == ' ' ? 1 : 0;
    return words;
}

// 以 "/" 或 "!" 开头(点了会被当成命令)或含看不见字符的按钮会被丢弃。
bool label_dropped(const std::string& label) {
    if (!label.empty() && (label.front() == '/' || label.front() == '!')) return true;
    for (std::size_t i = 0; i < label.size();) {
        if (is_invisible_code_point(next_code_point(label, i))) return true;
    }
    return false;
}

std::string string_field(const nlohmann::json& args, const char* key) {
    const auto it = args.find(key);
    return it != args.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

bool truthy(const nlohmann::json& args, const char* key) {
    const auto it = args.find(key);
    if (it == args.end()) return false;
    if (it->is_boolean()) return it->get<bool>();
    return it->is_string() && it->get<std::string>() == "true";
}

// actions 的一项:{"label": "..."},也接受直接给字符串。
std::string raw_label(const nlohmann::json& entry) {
    if (entry.is_string()) return entry.get<std::string>();
    if (entry.is_object()) return string_field(entry, "label");
    return {};
}

std::string receipt_text(const TaskCompleteCall& call) {
    std::string text = call.box_shown
        ? "Shown in the next-step box."
        : call.nothing_pending
            ? "Nothing is pending, so no box is drawn; the summary is shown as the turn's recap."
            : "No box is drawn for this call; the summary is shown as the turn's recap.";
    if (!call.advice.empty()) {
        text += " The advice that follows is for your next call.";
        for (const auto& line : call.advice) text += " " + line;
    }
    return text;
}

ToolResult execute_task_complete(const std::string& arguments_json,
                                 const ToolContext& /*ctx*/) {
    const TaskCompleteCall call = parse_task_complete_call(arguments_json);
    if (call.summary.empty()) {
        return ToolResult{
            "task_complete requires a non-empty summary describing what was accomplished",
            false};
    }

    ToolSummary sum;
    sum.verb = "complete";
    sum.object = "task";
    sum.icon = tool_icon("task_complete");
    sum.metrics.emplace_back("summary", call.summary);
    // 没有「下一步」框的界面(TUI)把问题接在总结后面显示,不能让它丢掉。
    if (call.box_shown && !call.ask.empty()) sum.metrics.emplace_back("ask", call.ask);

    nlohmann::json wrap_up = {{"recap", call.summary}};
    if (call.box_shown) {
        wrap_up["box"] = true;
        if (!call.ask.empty()) wrap_up["ask"] = call.ask;
        nlohmann::json actions = nlohmann::json::array();
        for (const auto& label : call.actions) actions.push_back({{"label", label}});
        if (!actions.empty()) wrap_up["actions"] = std::move(actions);
    }
    if (call.blocked) wrap_up["blocked"] = true;

    ToolResult r;
    r.output = receipt_text(call);
    r.success = true;
    r.summary = std::move(sum);
    r.metadata = nlohmann::json{{"wrap_up", std::move(wrap_up)}};
    return r;
}

} // namespace

TaskCompleteCall parse_task_complete_call(const std::string& arguments_json) {
    TaskCompleteCall call;
    nlohmann::json args;
    try {
        args = nlohmann::json::parse(arguments_json);
    } catch (const std::exception&) {
        // JSON 解析失败时 summary 保持为空,调用方走"缺参"分支。
        return call;
    }
    if (!args.is_object()) return call;

    std::string summary = utils::trim_ascii_copy(string_field(args, "summary"));
    if (summary.empty()) summary = utils::trim_ascii_copy(string_field(args, "recap"));
    call.summary = std::move(summary);
    if (call.summary.empty()) return call;

    const std::string ask = utils::trim_ascii_copy(string_field(args, "ask"));
    call.blocked = truthy(args, "blocked");
    const auto actions_it = args.find("actions");
    const nlohmann::json raw_actions = actions_it != args.end() && actions_it->is_array()
        ? *actions_it : nlohmann::json::array();
    call.nothing_pending = ask.empty() && raw_actions.empty() && !call.blocked;

    // 形状合法(有非空文字)的按钮;只有前两个会被画出来。
    std::vector<std::string> shaped;
    for (const auto& entry : raw_actions) {
        std::string label = single_line(raw_label(entry));
        if (!label.empty()) shaped.push_back(std::move(label));
    }
    const std::string first_raw = raw_actions.empty()
        ? std::string{} : single_line(raw_label(raw_actions.front()));
    // 推荐项(第一个按钮)无效时,其余按钮随它一起丢弃,问题也不显示(blocked 除外)。
    const bool first_ok = !first_raw.empty() && !label_dropped(first_raw);

    bool any_dropped = false;
    std::string long_label;
    for (std::size_t i = 0; i < shaped.size() && i < kMaxActions; ++i) {
        const std::string& label = shaped[i];
        if (label_dropped(label)) { any_dropped = true; continue; }
        if (long_label.empty() &&
            (code_point_count(label) > kLabelMaxChars || word_count(label) > kLabelMaxWords)) {
            long_label = label;
        }
        if (first_ok) call.actions.push_back(code_point_prefix(label, kLabelMaxChars));
    }

    call.box_shown = call.blocked || (!ask.empty() && first_ok);
    if (call.box_shown) {
        call.ask = ask;
    } else {
        call.actions.clear();
    }

    if (!call.nothing_pending && (ask.empty() || shaped.empty())) {
        call.advice.push_back(
            "The box drew no buttons: it needs ask and at least one entry in actions "
            "({\"label\": \"Yes, do it\"}); when nothing is left for the user, the summary "
            "alone (no ask, no actions, not blocked) is the whole call.");
    } else if (code_point_count(ask) > kAskMaxChars) {
        call.advice.push_back(
            "ask is the decision alone, in one short sentence (about 100 characters) with "
            "no reason or recommendation; this one ran " +
            std::to_string(code_point_count(ask)) + ".");
    }
    if (shaped.size() > kMaxActions) {
        call.advice.push_back(
            "actions is 1-2 buttons; this call had " + std::to_string(shaped.size()) +
            ", and every one past the first 2 is dropped.");
    }
    if (any_dropped) {
        call.advice.push_back(
            "A reply whose label starts with \"/\" or \"!\", or carries an invisible character "
            "such as an emoji variation selector or a zero-width joiner, is dropped (sent text "
            "would run a command or hide what it sends); when that reply was the "
            "recommendation, the other answers go with it, and the question too unless the "
            "call is blocked. Rewrite it without those.");
    }
    if (!long_label.empty()) {
        call.advice.push_back(
            "A reply label is a button, 1-3 words (4 at most, 24 characters); \"" +
            long_label + "\" is too long and the button clips it. Keep the label to the "
            "answer itself.");
    }
    if (code_point_count(call.summary) > kSummaryTargetChars) {
        call.advice.push_back(
            "summary is two or three sentences (about 300 characters); this one ran " +
            std::to_string(code_point_count(call.summary)) +
            ". Keep it to what is now true and where it stands; details belong in your "
            "closing message.");
    }
    return call;
}

std::string task_complete_plain_text(const std::string& arguments_json) {
    const TaskCompleteCall call = parse_task_complete_call(arguments_json);
    if (call.summary.empty()) return {};
    // summary 保留模型写的原文(含首尾换行),与旧版逐字一致。
    const auto args = nlohmann::json::parse(arguments_json, nullptr, false);
    std::string text = string_field(args, "summary");
    if (utils::trim_ascii_copy(text).empty()) text = string_field(args, "recap");
    if (!call.box_shown || call.ask.empty()) return text;
    text += "\n\n" + call.ask;
    std::string labels;
    for (const auto& label : call.actions) labels += (labels.empty() ? "" : " / ") + label;
    return labels.empty() ? text : text + " (" + labels + ")";
}

ToolImpl create_task_complete_tool() {
    ToolDef def;
    def.name = "task_complete";
    def.description = kToolDescription;
    const nlohmann::json action_schema = {
        {"type", "object"},
        {"properties", {
            {"label", {
                {"type", "string"},
                {"description",
                 "The button text, 1-3 words (4 only when three cannot say it) / 24 "
                 "characters, e.g. \"Yes, fix 1\", \"Fix 2 instead\", \"Not now\": a button, "
                 "not a sentence. A click sends it to you as the user's answer. No leading "
                 "/ or !."}
            }}
        }},
        {"required", nlohmann::json::array({"label"})}
    };
    def.parameters = {
        {"type", "object"},
        {"required", nlohmann::json::array({"summary"})},
        {"properties", {
            {"summary", {
                {"type", "string"},
                {"minLength", 1},
                {"description",
                 "Two or three short sentences to the user (about 300 characters): what is "
                 "now true or changed, then where it stands, including what is not done or "
                 "not verified. It must stand on its own: never point at anything else in "
                 "the conversation (\"listed below\", \"see above\", \"in my message\")."}
            }},
            {"ask", {
                {"type", "string"},
                {"description",
                 "The one question (at most 100 characters) when the next move is the "
                 "user's call: a choice, a go-ahead (to act on what this turn found, say), "
                 "an approval. The decision alone, with no reason or recommendation and "
                 "without listing the options the buttons give (actions[0] is your pick). "
                 "Omit it, and actions, when nothing waits on the user."}
            }},
            {"actions", {
                {"type", "array"},
                {"items", action_schema},
                {"description",
                 "1-2 reply buttons, recommendation first. They are answers to your ask: "
                 "the first accepts your recommendation, and another is there only when it "
                 "is a realistic alternative the user might pick instead, not a menu of "
                 "everything you mentioned. When blocked is true, they are the most likely "
                 "answers to your question."}
            }},
            {"blocked", {
                {"type", "boolean"},
                {"description",
                 "true when your work stops here until the user answers; omit when the "
                 "answer can wait."}
            }}
        }}
    };

    ToolImpl impl;
    impl.definition = def;
    impl.execute = execute_task_complete;
    // is_read_only=true 让 PermissionManager::should_auto_allow 在非 Yolo 模式下
    // 也直接放行(与 ask_user_question_tool 同样策略)。task 2.4 另外在
    // permissions.hpp 里明确写出按名放行兜底,避免依赖这个标志。
    impl.is_read_only = true;
    impl.source = ToolSource::Builtin;
    return impl;
}

} // namespace acecode
