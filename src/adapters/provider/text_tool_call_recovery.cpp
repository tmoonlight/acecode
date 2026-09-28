#include "text_tool_call_recovery.hpp"

#include "markdown_fence_tracker.hpp"
#include "llm/tool_protocol_names.hpp"
#include "utils/encoding.hpp"
#include "utils/logger.hpp"
#include "utils/sha1.hpp"
#include "utils/uuid.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <set>
#include <utility>

namespace acecode {
namespace {

// ---- 语法里的字面量 ------------------------------------------------------

constexpr std::string_view kFunctionCallsOpen = "<function_calls>";
constexpr std::string_view kFunctionCallsClose = "</function_calls>";
constexpr std::string_view kDotsOpen = "<dots_function_call>";
constexpr std::string_view kDotsClose = "</dots_function_call>";
constexpr std::string_view kInvokeOpen = "<invoke";
constexpr std::string_view kInvokeClose = "</invoke>";
constexpr std::string_view kParameterOpen = "<parameter";
constexpr std::string_view kParameterClose = "</parameter>";
constexpr std::string_view kToolCallOpen = "<tool_call>";
constexpr std::string_view kToolCallClose = "</tool_call>";
constexpr std::string_view kFunctionEqOpen = "<function=";
constexpr std::string_view kFunctionClose = "</function>";
constexpr std::string_view kParameterEqOpen = "<parameter=";
constexpr std::string_view kNameAttr = "name=";
constexpr std::string_view kDsmlMarker = u8"｜DSML｜";

// 行首开标签(probe 必须始终是其中某一个的前缀)。两两之间没有前缀关系。
constexpr std::array<std::string_view, 4> kOpeners{{
    kFunctionCallsOpen,
    kDotsOpen,
    kInvokeOpen,
    kToolCallOpen,
}};

// 块后允许出现、直接忽略的特殊 token(各家聊天模板的结束符;DSML 的
// end_of_sentence 通常已被 DSML 过滤器吞掉,这里兜底)。
constexpr std::array<std::string_view, 6> kSpecialTokens{{
    "<|im_end|>",
    "<|eot_id|>",
    "<|endoftext|>",
    "</s>",
    "<|end|>",
    u8"<｜end▁of▁sentence｜>",
}};

// 孤立的外壳闭合标签:裸 invoke 后多一个 `</function_calls>`(模板残留)。
constexpr std::array<std::string_view, 2> kOrphanCloses{{
    kFunctionCallsClose,
    kDotsClose,
}};

constexpr std::size_t kMaxNameLength = 128;
// 标签头(`<invoke` 之后到 `>`)的长度上限:名字 128 + 少量空白与属性名。
// 超过即判定为不是调用,防止病态输入让扣住态无限期拖住正文。
constexpr std::size_t kMaxHeadLength = 512;
constexpr std::size_t kRawExcerptBytes = 4096;
constexpr std::size_t kMaxListedTools = 100;

bool is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
           c == '\v';
}

bool is_blank(std::string_view text) {
    for (char c : text) {
        if (!is_ws(c)) return false;
    }
    return true;
}

bool starts_with_at(std::string_view text, std::size_t pos, std::string_view lit) {
    return pos <= text.size() && lit.size() <= text.size() - pos &&
           text.compare(pos, lit.size(), lit) == 0;
}

bool is_prefix_of(std::string_view token, std::string_view probe) {
    return probe.size() <= token.size() &&
           token.compare(0, probe.size(), probe) == 0;
}

enum class Lit { Full, Partial, None };

// buf[pos..] 与字面量比较:完整命中 / buf 在字面量中途结束(还需更多数据)/ 不符。
Lit match_lit(const std::string& buf, std::size_t pos, std::string_view lit) {
    const std::size_t avail = buf.size() - pos;
    const std::size_t n = std::min(avail, lit.size());
    if (buf.compare(pos, n, lit.data(), n) != 0) return Lit::None;
    return n == lit.size() ? Lit::Full : Lit::Partial;
}

std::string trim_ws(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && is_ws(text[begin])) ++begin;
    while (end > begin && is_ws(text[end - 1])) --end;
    return std::string(text.substr(begin, end - begin));
}

// VALUE 只去掉开头 1 个换行与结尾 1 个换行(与 vLLM qwen3coder parser 的约定
// 一致),其余原样保留,不做 XML 实体反转义。
std::string strip_one_newline_each_side(std::string_view value) {
    if (starts_with_at(value, 0, "\r\n")) {
        value.remove_prefix(2);
    } else if (!value.empty() && value.front() == '\n') {
        value.remove_prefix(1);
    }
    if (value.size() >= 2 && value.substr(value.size() - 2) == "\r\n") {
        value.remove_suffix(2);
    } else if (!value.empty() && value.back() == '\n') {
        value.remove_suffix(1);
    }
    return std::string(value);
}

std::string join(const std::vector<std::string>& items, std::string_view sep,
                 std::size_t max_items = std::string::npos) {
    std::string out;
    const std::size_t listed = std::min(items.size(), max_items);
    for (std::size_t i = 0; i < listed; ++i) {
        if (i) out.append(sep);
        out += items[i];
    }
    if (items.size() > listed) {
        out += " (+" + std::to_string(items.size() - listed) + " more)";
    }
    return out;
}

// ---- 请求工具表:名字规范化与参数 schema ----------------------------------

class ToolIndex {
public:
    explicit ToolIndex(const std::vector<ToolDef>& tools) : tools_(tools) {}

    bool empty() const { return tools_.empty(); }
    const ToolDef& at(std::size_t i) const { return tools_[i]; }

    // 模型写的名字 → 请求表下标。依次:精确 → 原生名 / 模型侧名互认(同 DSML
    // add_allowed_tool_name)→ ASCII 大小写不敏感且候选唯一。
    std::optional<std::size_t> resolve(const std::string& written) const {
        if (written.empty()) return std::nullopt;
        for (std::size_t i = 0; i < tools_.size(); ++i) {
            if (tools_[i].name == written) return i;
        }
        std::set<std::size_t> alias_hits;
        std::set<std::size_t> ci_hits;
        for (std::size_t i = 0; i < tools_.size(); ++i) {
            const std::string& name = tools_[i].name;
            const std::string model_name = model_tool_name_for_native(name);
            const auto native_name = native_tool_name_for_public_alias(name);
            if (written == model_name || (native_name && written == *native_name)) {
                alias_hits.insert(i);
            }
            if (ascii_iequals(written, name) || ascii_iequals(written, model_name) ||
                (native_name && ascii_iequals(written, *native_name))) {
                ci_hits.insert(i);
            }
        }
        if (alias_hits.size() == 1) return *alias_hits.begin();
        if (!alias_hits.empty()) return std::nullopt;
        if (ci_hits.size() == 1) return *ci_hits.begin();
        return std::nullopt;
    }

    std::string normalized_name(const std::string& written) const {
        const auto idx = resolve(written);
        return idx ? tools_[*idx].name : written;
    }

    const nlohmann::json* property_schema(std::size_t idx,
                                          const std::string& param) const {
        const auto& params = tools_[idx].parameters;
        if (!params.is_object()) return nullptr;
        const auto props = params.find("properties");
        if (props == params.end() || !props->is_object()) return nullptr;
        const auto it = props->find(param);
        if (it == props->end()) return nullptr;
        return &*it;
    }

private:
    std::vector<ToolDef> tools_;
};

void collect_schema_types(const nlohmann::json& schema,
                          std::set<std::string>& types,
                          int depth = 0) {
    if (!schema.is_object() || depth > 3) return;
    const auto type = schema.find("type");
    if (type != schema.end()) {
        if (type->is_string()) {
            types.insert(type->get<std::string>());
        } else if (type->is_array()) {
            for (const auto& item : *type) {
                if (item.is_string()) types.insert(item.get<std::string>());
            }
        }
    }
    for (const char* key : {"anyOf", "oneOf"}) {
        const auto it = schema.find(key);
        if (it == schema.end() || !it->is_array()) continue;
        for (const auto& sub : *it) collect_schema_types(sub, types, depth + 1);
    }
}

bool json_matches_type(const nlohmann::json& value, const std::string& type) {
    if (type == "integer") return value.is_number_integer();
    if (type == "number") return value.is_number();
    if (type == "boolean") return value.is_boolean();
    if (type == "array") return value.is_array();
    if (type == "object") return value.is_object();
    if (type == "null") return value.is_null();
    return false;
}

// 未声明 / 无类型的参数:以 { 或 [ 开头且能解析成 JSON 就用 JSON,否则当字符串。
nlohmann::json untyped_parameter_value(const std::string& raw) {
    const std::string trimmed = trim_ws(raw);
    if (!trimmed.empty() && (trimmed.front() == '{' || trimmed.front() == '[')) {
        auto parsed = nlohmann::json::parse(trimmed, nullptr, false);
        if (!parsed.is_discarded()) return parsed;
    }
    return nlohmann::json(raw);
}

// 按 parameters.properties[P] 转换参数类型。失败时 error 为可写进纠正提示的英文。
bool coerce_parameter(const nlohmann::json* schema,
                      const std::string& tool,
                      const std::string& param,
                      const std::string& raw,
                      nlohmann::json& out,
                      std::string& error) {
    if (!schema) {
        LOG_DEBUG("text tool call: undeclared parameter \"" + param +
                  "\" of tool \"" + tool + "\" passed through");
        out = untyped_parameter_value(raw);
        return true;
    }
    std::set<std::string> types;
    collect_schema_types(*schema, types);
    if (types.empty()) {
        out = untyped_parameter_value(raw);
        return true;
    }
    std::vector<std::string> structured;
    for (const char* t : {"integer", "number", "boolean", "array", "object"}) {
        if (types.count(t)) structured.push_back(t);
    }
    if (structured.empty()) {
        // string、或只有 string / null 的联合:原样作为字符串。
        out = nlohmann::json(raw);
        return true;
    }
    const std::string trimmed = trim_ws(raw);
    auto parsed = nlohmann::json::parse(trimmed, nullptr, false);
    if (!parsed.is_discarded()) {
        for (const auto& t : types) {
            if (json_matches_type(parsed, t)) {
                out = std::move(parsed);
                return true;
            }
        }
    }
    if (types.count("string")) {
        out = nlohmann::json(raw);
        return true;
    }
    error = "parameter \"" + param + "\" of tool \"" + tool + "\" expects " +
            join(structured, " or ") + ", got \"" +
            truncate_utf8_prefix(trimmed, 60) + "\"";
    return false;
}

// ---- 增量解析器 ------------------------------------------------------------

struct ParsedParam {
    std::string name;
    std::string value;
};

struct ParsedCall {
    std::string format;
    std::string name; // 模型写的原名
    std::vector<ParsedParam> params;
    bool json_form = false;
    nlohmann::json json_arguments = nlohmann::json::object();
    std::string json_error;
};

// 在 buf 里从 search_from 起找结束标签。没找到时把 search_from 推进到「结尾
// 可能是标签前缀」的位置 —— 下次只在新数据上接着找,整体线性。
std::size_t search_close(const std::string& buf,
                         std::size_t& search_from,
                         std::string_view tag,
                         std::size_t& scanned) {
    const std::size_t found = buf.find(tag.data(), search_from, tag.size());
    if (found != std::string::npos) {
        scanned += found + tag.size() - search_from;
        return found;
    }
    scanned += buf.size() - search_from;
    std::size_t next = buf.size();
    std::size_t lo = buf.size() >= tag.size() - 1 ? buf.size() - (tag.size() - 1) : 0;
    lo = std::max(lo, search_from);
    for (std::size_t j = lo; j < buf.size(); ++j) {
        if (buf[j] == tag[0] &&
            buf.compare(j, buf.size() - j, tag.data(), buf.size() - j) == 0) {
            next = j;
            break;
        }
    }
    search_from = next;
    return std::string::npos;
}

class IncrementalParser {
public:
    enum class Status { NeedMore, NotACall };

    void reset() { *this = IncrementalParser{}; }

    // 从上次停下的位置继续解析 buf(buf 只会在尾部追加)。
    Status advance(const std::string& buf) {
        for (;;) {
            const Step step = this->step(buf);
            if (step == Step::Continue) continue;
            if (step == Step::NeedMore) return Status::NeedMore;
            return Status::NotACall;
        }
    }

    // EOF 定案:语法完整返回 true(含「只缺外壳闭合标签」的容错)。
    bool complete_at_eof(const std::string& buf) const {
        std::size_t q = p_;
        while (q < buf.size() && is_ws(buf[q])) ++q;
        const std::string_view rest(buf.data() + q, buf.size() - q);
        if (phase_ == Phase::Tail) {
            if (rest.empty()) return true;
            for (auto token : kSpecialTokens) {
                if (is_prefix_of(token, rest)) return true;
            }
            for (auto token : kOrphanCloses) {
                if (is_prefix_of(token, rest)) return true;
            }
            return false;
        }
        if (phase_ == Phase::WrapperBody && wrapper_invokes_ > 0) {
            // 网关把外壳闭合标签设成了 stop sequence:invoke 都闭合了,只缺外壳。
            return rest.empty() || is_prefix_of(wrapper_close_, rest);
        }
        return false;
    }

    std::string missing_close() const {
        switch (phase_) {
            case Phase::WrapperBody:
                return std::string(wrapper_close_);
            case Phase::ParamValue:
                return std::string(kParameterClose);
            case Phase::ToolCallBody:
            case Phase::ToolCallJson:
            case Phase::ToolCallClose:
                return std::string(kToolCallClose);
            case Phase::QwenFnHead:
            case Phase::QwenFnBody:
            case Phase::QwenParamHead:
                return std::string(kFunctionClose);
            default:
                return std::string(kInvokeClose);
        }
    }

    std::size_t violation() const { return violation_; }
    const std::vector<ParsedCall>& calls() const { return calls_; }
    const std::string& pending_name() const { return current_.name; }
    std::string format() const {
        return calls_.empty() ? block_format_ : calls_.front().format;
    }
    std::size_t scanned() const { return scanned_; }

private:
    enum class Phase {
        Top,           // 第一个块的开标签
        Tail,          // 至少一个块已闭合:空白 / 特殊 token / 孤立外壳闭合 / 下一个块
        WrapperBody,   // <function_calls> / <dots_function_call> 内部
        InvokeHead,    // "<invoke" 之后:ws+ name= Q NAME Q ws* >
        InvokeBody,    // <parameter …> 或 </invoke>
        ParamHead,     // "<parameter" 之后:ws+ name= Q PNAME Q ws* >
        ParamValue,    // 找 </parameter>
        ToolCallBody,  // <tool_call> 之后:{json} 或 <function=
        ToolCallJson,  // 找 </tool_call>
        QwenFnHead,    // "<function=" 之后:NAME >
        QwenFnBody,    // <parameter=…> 或 </function>
        QwenParamHead, // "<parameter=" 之后:PNAME >
        ToolCallClose, // </function> 之后:ws* </tool_call>
    };
    enum class Step { Continue, NeedMore, Violation };

    Step violate(std::size_t at) {
        violation_ = at;
        return Step::Violation;
    }

    void skip_ws(const std::string& buf) {
        const std::size_t start = p_;
        while (p_ < buf.size() && is_ws(buf[p_])) ++p_;
        scanned_ += p_ - start;
    }

    void start_block(std::string_view opener) {
        p_ += opener.size();
        scanned_ += opener.size();
        if (opener == kFunctionCallsOpen || opener == kDotsOpen) {
            block_format_ = opener == kFunctionCallsOpen ? "function_calls"
                                                         : "dots_function_call";
            wrapper_close_ = opener == kFunctionCallsOpen ? kFunctionCallsClose
                                                          : kDotsClose;
            wrapper_invokes_ = 0;
            in_wrapper_ = true;
            phase_ = Phase::WrapperBody;
        } else if (opener == kInvokeOpen) {
            block_format_ = "invoke";
            in_wrapper_ = false;
            phase_ = Phase::InvokeHead;
        } else {
            block_format_ = "tool_call_json";
            in_wrapper_ = false;
            phase_ = Phase::ToolCallBody;
        }
    }

    // ws+ "name=" Q NAME Q ws* ">"
    Step parse_quoted_head(const std::string& buf, std::string& name) {
        std::size_t i = p_;
        const auto done = [&](Step s) {
            scanned_ += i - p_;
            return s;
        };
        while (i < buf.size() && is_ws(buf[i])) {
            ++i;
            if (i - p_ > kMaxHeadLength) return done(violate(i));
        }
        if (i == buf.size()) return done(Step::NeedMore);
        if (i == p_) return done(violate(i));
        const Lit attr = match_lit(buf, i, kNameAttr);
        if (attr == Lit::None) return done(violate(i));
        if (attr == Lit::Partial) return done(Step::NeedMore);
        i += kNameAttr.size();
        if (i == buf.size()) return done(Step::NeedMore);
        const char quote = buf[i];
        if (quote != '"' && quote != '\'') return done(violate(i));
        ++i;
        const std::size_t name_start = i;
        while (i < buf.size() && buf[i] != quote) {
            const char c = buf[i];
            if (c == '<' || c == '>' || c == '\n' || c == '\r' || c == '"' ||
                c == '\'') {
                return done(violate(i));
            }
            ++i;
            if (i - name_start > kMaxNameLength) return done(violate(i));
        }
        if (i == buf.size()) return done(Step::NeedMore);
        if (i == name_start) return done(violate(i));
        std::string parsed(buf, name_start, i - name_start);
        ++i;
        while (i < buf.size() && is_ws(buf[i])) {
            ++i;
            if (i - p_ > kMaxHeadLength) return done(violate(i));
        }
        if (i == buf.size()) return done(Step::NeedMore);
        if (buf[i] != '>') return done(violate(i));
        ++i;
        scanned_ += i - p_;
        p_ = i;
        name = std::move(parsed);
        return Step::Continue;
    }

    // NAME ">"(Qwen 形式,不带引号)
    Step parse_unquoted_head(const std::string& buf, std::string& name) {
        std::size_t i = p_;
        const auto done = [&](Step s) {
            scanned_ += i - p_;
            return s;
        };
        while (i < buf.size() && buf[i] != '>') {
            const char c = buf[i];
            if (c == '<' || c == '"' || c == '\'' || is_ws(c)) {
                return done(violate(i));
            }
            ++i;
            if (i - p_ > kMaxNameLength) return done(violate(i));
        }
        if (i == buf.size()) return done(Step::NeedMore);
        if (i == p_) return done(violate(i));
        name.assign(buf, p_, i - p_);
        ++i;
        scanned_ += i - p_;
        p_ = i;
        return Step::Continue;
    }

    void finish_call() {
        calls_.push_back(std::move(current_));
        current_ = ParsedCall{};
    }

    // 在 p_ 处尝试一组字面量:完整命中返回其下标;部分命中置 partial。
    template <std::size_t N>
    std::optional<std::size_t> match_any(const std::string& buf,
                                         const std::array<std::string_view, N>& lits,
                                         bool& partial) const {
        for (std::size_t i = 0; i < N; ++i) {
            const Lit m = match_lit(buf, p_, lits[i]);
            if (m == Lit::Full) return i;
            if (m == Lit::Partial) partial = true;
        }
        return std::nullopt;
    }

    Step step(const std::string& buf) {
        switch (phase_) {
            case Phase::Top:
            case Phase::Tail: {
                if (phase_ == Phase::Tail) skip_ws(buf);
                if (p_ >= buf.size()) return Step::NeedMore;
                bool partial = false;
                if (auto op = match_any(buf, kOpeners, partial)) {
                    start_block(kOpeners[*op]);
                    return Step::Continue;
                }
                if (phase_ == Phase::Tail) {
                    if (auto t = match_any(buf, kSpecialTokens, partial)) {
                        p_ += kSpecialTokens[*t].size();
                        scanned_ += kSpecialTokens[*t].size();
                        return Step::Continue;
                    }
                    if (auto t = match_any(buf, kOrphanCloses, partial)) {
                        p_ += kOrphanCloses[*t].size();
                        scanned_ += kOrphanCloses[*t].size();
                        return Step::Continue;
                    }
                }
                if (partial) return Step::NeedMore;
                return violate(p_);
            }
            case Phase::WrapperBody: {
                skip_ws(buf);
                if (p_ >= buf.size()) return Step::NeedMore;
                const Lit invoke = match_lit(buf, p_, kInvokeOpen);
                if (invoke == Lit::Full) {
                    p_ += kInvokeOpen.size();
                    scanned_ += kInvokeOpen.size();
                    phase_ = Phase::InvokeHead;
                    return Step::Continue;
                }
                const Lit close = match_lit(buf, p_, wrapper_close_);
                if (close == Lit::Full) {
                    if (wrapper_invokes_ == 0) return violate(p_);
                    p_ += wrapper_close_.size();
                    scanned_ += wrapper_close_.size();
                    in_wrapper_ = false;
                    phase_ = Phase::Tail;
                    return Step::Continue;
                }
                if (invoke == Lit::Partial || close == Lit::Partial) {
                    return Step::NeedMore;
                }
                return violate(p_);
            }
            case Phase::InvokeHead: {
                std::string name;
                const Step s = parse_quoted_head(buf, name);
                if (s != Step::Continue) return s;
                current_ = ParsedCall{};
                current_.format = in_wrapper_ ? block_format_ : "invoke";
                current_.name = std::move(name);
                phase_ = Phase::InvokeBody;
                return Step::Continue;
            }
            case Phase::InvokeBody: {
                skip_ws(buf);
                if (p_ >= buf.size()) return Step::NeedMore;
                const Lit param = match_lit(buf, p_, kParameterOpen);
                if (param == Lit::Full) {
                    p_ += kParameterOpen.size();
                    scanned_ += kParameterOpen.size();
                    phase_ = Phase::ParamHead;
                    return Step::Continue;
                }
                const Lit close = match_lit(buf, p_, kInvokeClose);
                if (close == Lit::Full) {
                    p_ += kInvokeClose.size();
                    scanned_ += kInvokeClose.size();
                    finish_call();
                    if (in_wrapper_) {
                        ++wrapper_invokes_;
                        phase_ = Phase::WrapperBody;
                    } else {
                        phase_ = Phase::Tail;
                    }
                    return Step::Continue;
                }
                if (param == Lit::Partial || close == Lit::Partial) {
                    return Step::NeedMore;
                }
                return violate(p_);
            }
            case Phase::ParamHead: {
                std::string name;
                const Step s = parse_quoted_head(buf, name);
                if (s != Step::Continue) return s;
                begin_value(std::move(name), Phase::InvokeBody);
                return Step::Continue;
            }
            case Phase::QwenParamHead: {
                std::string name;
                const Step s = parse_unquoted_head(buf, name);
                if (s != Step::Continue) return s;
                begin_value(std::move(name), Phase::QwenFnBody);
                return Step::Continue;
            }
            case Phase::ParamValue: {
                const std::size_t close =
                    search_close(buf, search_from_, kParameterClose, scanned_);
                if (close == std::string::npos) return Step::NeedMore;
                current_.params.push_back(ParsedParam{
                    std::move(param_name_),
                    strip_one_newline_each_side(std::string_view(buf).substr(
                        value_start_, close - value_start_)),
                });
                param_name_.clear();
                p_ = close + kParameterClose.size();
                phase_ = value_return_;
                return Step::Continue;
            }
            case Phase::ToolCallBody: {
                skip_ws(buf);
                if (p_ >= buf.size()) return Step::NeedMore;
                if (buf[p_] == '{') {
                    json_start_ = p_;
                    search_from_ = p_;
                    phase_ = Phase::ToolCallJson;
                    return Step::Continue;
                }
                const Lit fn = match_lit(buf, p_, kFunctionEqOpen);
                if (fn == Lit::Full) {
                    p_ += kFunctionEqOpen.size();
                    scanned_ += kFunctionEqOpen.size();
                    phase_ = Phase::QwenFnHead;
                    return Step::Continue;
                }
                if (fn == Lit::Partial) return Step::NeedMore;
                return violate(p_);
            }
            case Phase::ToolCallJson: {
                const std::size_t close =
                    search_close(buf, search_from_, kToolCallClose, scanned_);
                if (close == std::string::npos) return Step::NeedMore;
                if (!accept_json_call(std::string_view(buf).substr(
                        json_start_, close - json_start_))) {
                    return violate(json_start_);
                }
                p_ = close + kToolCallClose.size();
                phase_ = Phase::Tail;
                return Step::Continue;
            }
            case Phase::QwenFnHead: {
                std::string name;
                const Step s = parse_unquoted_head(buf, name);
                if (s != Step::Continue) return s;
                current_ = ParsedCall{};
                current_.format = "tool_call_function";
                current_.name = std::move(name);
                block_format_ = "tool_call_function";
                phase_ = Phase::QwenFnBody;
                return Step::Continue;
            }
            case Phase::QwenFnBody: {
                skip_ws(buf);
                if (p_ >= buf.size()) return Step::NeedMore;
                const Lit param = match_lit(buf, p_, kParameterEqOpen);
                if (param == Lit::Full) {
                    p_ += kParameterEqOpen.size();
                    scanned_ += kParameterEqOpen.size();
                    phase_ = Phase::QwenParamHead;
                    return Step::Continue;
                }
                const Lit close = match_lit(buf, p_, kFunctionClose);
                if (close == Lit::Full) {
                    p_ += kFunctionClose.size();
                    scanned_ += kFunctionClose.size();
                    finish_call();
                    phase_ = Phase::ToolCallClose;
                    return Step::Continue;
                }
                if (param == Lit::Partial || close == Lit::Partial) {
                    return Step::NeedMore;
                }
                return violate(p_);
            }
            case Phase::ToolCallClose: {
                skip_ws(buf);
                if (p_ >= buf.size()) return Step::NeedMore;
                const Lit close = match_lit(buf, p_, kToolCallClose);
                if (close == Lit::Full) {
                    p_ += kToolCallClose.size();
                    scanned_ += kToolCallClose.size();
                    phase_ = Phase::Tail;
                    return Step::Continue;
                }
                if (close == Lit::Partial) return Step::NeedMore;
                return violate(p_);
            }
        }
        return violate(p_);
    }

    void begin_value(std::string name, Phase return_to) {
        param_name_ = std::move(name);
        value_start_ = p_;
        search_from_ = p_;
        value_return_ = return_to;
        phase_ = Phase::ParamValue;
    }

    // Hermes:{"name": string, "arguments"|"parameters": object | 内容是 JSON object 的字符串}。
    // 结构不符(不是 object / 没有 name)按「不是调用」处理;arguments 形状不对
    // 记 json_error,由校验阶段报 bad_param。
    bool accept_json_call(std::string_view text) {
        auto parsed = nlohmann::json::parse(trim_ws(text), nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object()) return false;
        const auto name = parsed.find("name");
        if (name == parsed.end() || !name->is_string() ||
            name->get<std::string>().empty()) {
            return false;
        }
        ParsedCall call;
        call.format = "tool_call_json";
        call.name = name->get<std::string>();
        call.json_form = true;
        auto args = parsed.find("arguments");
        if (args == parsed.end()) args = parsed.find("parameters");
        if (args == parsed.end() || args->is_null()) {
            call.json_arguments = nlohmann::json::object();
        } else if (args->is_object()) {
            call.json_arguments = *args;
        } else if (args->is_string()) {
            auto inner = nlohmann::json::parse(args->get<std::string>(), nullptr, false);
            if (!inner.is_discarded() && inner.is_object()) {
                call.json_arguments = std::move(inner);
            } else {
                call.json_error = "arguments of tool \"" + call.name +
                                  "\" must be a JSON object";
            }
        } else {
            call.json_error = "arguments of tool \"" + call.name +
                              "\" must be a JSON object";
        }
        calls_.push_back(std::move(call));
        return true;
    }

    Phase phase_ = Phase::Top;
    std::size_t p_ = 0;
    std::size_t violation_ = 0;
    std::size_t scanned_ = 0;

    std::string block_format_;
    std::string_view wrapper_close_;
    std::size_t wrapper_invokes_ = 0;
    bool in_wrapper_ = false;

    ParsedCall current_;
    std::vector<ParsedCall> calls_;

    std::string param_name_;
    std::size_t value_start_ = 0;
    std::size_t search_from_ = 0;
    Phase value_return_ = Phase::InvokeBody;
    std::size_t json_start_ = 0;
};

std::string synthesize_text_tool_call_id(const std::string& id_scope,
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
    return "call_text_" + sha1_hex(fingerprint).substr(0, 24);
}

// 把一个语法完整的调用校验成原生 ToolCall。失败时填 reason / error。
bool validate_call(const ParsedCall& call,
                   const ToolIndex& index,
                   ToolCall& out,
                   std::string& reason,
                   std::string& error) {
    const auto idx = index.resolve(call.name);
    if (!idx) {
        reason = "unknown_tool";
        error = "tool \"" + call.name + "\" is not available";
        return false;
    }
    const std::string& tool_name = index.at(*idx).name;
    nlohmann::json arguments = nlohmann::json::object();
    if (call.json_form) {
        if (!call.json_error.empty()) {
            reason = "bad_param";
            error = call.json_error;
            return false;
        }
        arguments = call.json_arguments;
    } else {
        std::set<std::string> seen;
        for (const auto& param : call.params) {
            if (!seen.insert(param.name).second) {
                reason = "bad_param";
                error = "parameter \"" + param.name + "\" of tool \"" + tool_name +
                        "\" is given more than once";
                return false;
            }
            nlohmann::json value;
            if (!coerce_parameter(index.property_schema(*idx, param.name), tool_name,
                                  param.name, param.value, value, error)) {
                reason = "bad_param";
                return false;
            }
            arguments[param.name] = std::move(value);
        }
    }
    out.function_name = tool_name;
    out.function_arguments = arguments.dump();
    return true;
}

// 回显比对用的尽力而为版本:名字能规范就规范,参数转换失败就退回字符串。
ToolCall candidate_call(const ParsedCall& call, const ToolIndex& index) {
    ToolCall out;
    const auto idx = index.resolve(call.name);
    out.function_name = idx ? index.at(*idx).name : call.name;
    nlohmann::json arguments = nlohmann::json::object();
    if (call.json_form) {
        arguments = call.json_arguments;
    } else {
        for (const auto& param : call.params) {
            nlohmann::json value;
            std::string ignored;
            if (!idx || !coerce_parameter(index.property_schema(*idx, param.name),
                                          out.function_name, param.name,
                                          param.value, value, ignored)) {
                value = param.value;
            }
            arguments[param.name] = std::move(value);
        }
    }
    out.function_arguments = arguments.dump();
    return out;
}

} // namespace

// ---- 流式过滤器 ------------------------------------------------------------

struct TextToolCallStreamFilter::Impl {
    explicit Impl(const std::vector<ToolDef>& tools) : index(tools) { reset(); }

    ToolIndex index;
    std::string id_scope;
    MarkdownFenceTracker fence;
    std::string probe;
    std::string held;
    bool capturing = false;
    bool visible_nonblank = false;
    bool prefix_blank = true;
    IncrementalParser parser;
    std::size_t scanned_total = 0;

    void reset() {
        id_scope = generate_uuid_v7();
        fence.reset();
        probe.clear();
        held.clear();
        capturing = false;
        visible_nonblank = false;
        prefix_blank = true;
        parser.reset();
        scanned_total = 0;
    }

    void emit(char c, std::string& out) {
        out.push_back(c);
        fence.feed(c);
        if (!is_ws(c)) visible_nonblank = true;
    }

    void emit(std::string_view text, std::string& out) {
        for (char c : text) emit(c, out);
    }

    bool opener_can_start_here() const {
        return fence.at_line_prefix() && !fence.in_fence() &&
               !fence.line_opening_fence();
    }

    static bool probe_is_opener_prefix(std::string_view probe_text) {
        for (auto opener : kOpeners) {
            if (is_prefix_of(opener, probe_text)) return true;
        }
        return false;
    }

    static bool probe_is_opener(std::string_view probe_text) {
        for (auto opener : kOpeners) {
            if (probe_text == opener) return true;
        }
        return false;
    }

    void end_capture() {
        scanned_total += parser.scanned();
        parser.reset();
        held.clear();
        capturing = false;
    }

    // 语法偏离:释放 held[0 .. max(第一行行尾, 违规行行首)),其余部分返回给调用方
    // 重新扫描。每次至少消耗一行,既不会死循环,也不会漏掉后面真正的调用。
    std::string release_held(std::string& out) {
        const std::size_t violation = std::min(parser.violation(), held.size());
        const std::size_t first_nl = held.find('\n');
        const std::size_t first_end =
            first_nl == std::string::npos ? held.size() : first_nl + 1;
        std::size_t violation_line_start = 0;
        if (violation > 0) {
            const std::size_t nl = held.rfind('\n', violation - 1);
            violation_line_start = nl == std::string::npos ? 0 : nl + 1;
        }
        const std::size_t cut =
            std::min(held.size(), std::max(first_end, violation_line_start));
        emit(std::string_view(held).substr(0, cut), out);
        std::string rest = held.substr(cut);
        end_capture();
        return rest;
    }

    std::string push(std::string_view chunk) {
        std::string out;
        out.reserve(chunk.size());
        std::string pending;
        std::string_view input = chunk;
        std::size_t pos = 0;
        while (pos < input.size()) {
            if (capturing) {
                held.append(input.data() + pos, input.size() - pos);
                pos = input.size();
                if (parser.advance(held) == IncrementalParser::Status::NotACall) {
                    std::string rest = release_held(out);
                    pending = std::move(rest);
                    input = pending;
                    pos = 0;
                }
                continue;
            }

            const char c = input[pos];
            if (!probe.empty()) {
                std::string next = probe;
                next.push_back(c);
                if (probe_is_opener_prefix(next)) {
                    probe.push_back(c);
                    ++pos;
                    if (probe_is_opener(probe)) {
                        held = std::move(probe);
                        probe.clear();
                        capturing = true;
                        prefix_blank = !visible_nonblank;
                        parser.reset();
                    }
                    continue;
                }
                // probe 偏离了所有开标签:当正文放出,当前字节重新处理。
                std::string released = std::move(probe);
                probe.clear();
                emit(released, out);
                continue;
            }

            if (c == '<' && opener_can_start_here()) {
                probe.push_back(c);
                ++pos;
                continue;
            }
            emit(c, out);
            ++pos;
        }
        return out;
    }

    TextToolCallRecoveryResult finish() {
        TextToolCallRecoveryResult result;
        for (;;) {
            if (!capturing) {
                if (!probe.empty()) {
                    // probe 残留(例如结尾刚好是 `<invo`)→ 作为正文返回。
                    std::string released = std::move(probe);
                    probe.clear();
                    emit(released, result.visible_text);
                }
                break;
            }
            if (parser.advance(held) == IncrementalParser::Status::NotACall) {
                std::string rest = release_held(result.visible_text);
                result.visible_text += push(rest);
                continue;
            }
            finalize_capture(result);
            break;
        }
        return result;
    }

    void finalize_capture(TextToolCallRecoveryResult& result) {
        auto& diag = result.diagnostic;
        const bool complete = parser.complete_at_eof(held);
        const auto& calls = parser.calls();
        diag.format = parser.format();
        diag.raw_excerpt = truncate_utf8_prefix(held, kRawExcerptBytes, "");
        for (const auto& call : calls) diag.attempted_tools.push_back(call.name);
        if (!complete && !parser.pending_name().empty()) {
            diag.attempted_tools.push_back(parser.pending_name());
        }
        for (const auto& call : calls) {
            result.candidate_calls.push_back(candidate_call(call, index));
        }

        diag.outcome = TextToolCallDiagnostic::Outcome::Rejected;
        if (!complete) {
            diag.reason = "truncated";
            diag.error = "response ended inside a text tool call (missing " +
                         parser.missing_close() + ")";
        } else if (!prefix_blank) {
            diag.reason = "prose_prefix";
            diag.error = "a tool call was written as text after prose; tool calls "
                         "must go through the native interface";
        } else if (index.empty()) {
            diag.reason = "unknown_tool";
            diag.error = "no tools are available for this request";
        } else {
            std::vector<ToolCall> tool_calls;
            std::string reason;
            std::string error;
            bool ok = true;
            for (std::size_t i = 0; i < calls.size(); ++i) {
                ToolCall call;
                if (!validate_call(calls[i], index, call, reason, error)) {
                    ok = false;
                    break;
                }
                call.id = synthesize_text_tool_call_id(
                    id_scope, i, call.function_name, call.function_arguments);
                tool_calls.push_back(std::move(call));
            }
            if (ok) {
                diag.outcome = TextToolCallDiagnostic::Outcome::Recovered;
                diag.recovered_count = static_cast<int>(tool_calls.size());
                result.tool_calls = std::move(tool_calls);
            } else {
                diag.reason = std::move(reason);
                diag.error = std::move(error);
            }
        }
        end_capture();
    }
};

TextToolCallStreamFilter::TextToolCallStreamFilter(const std::vector<ToolDef>& tools)
    : impl_(std::make_unique<Impl>(tools)) {}

TextToolCallStreamFilter::~TextToolCallStreamFilter() = default;
TextToolCallStreamFilter::TextToolCallStreamFilter(TextToolCallStreamFilter&&) noexcept =
    default;
TextToolCallStreamFilter& TextToolCallStreamFilter::operator=(
    TextToolCallStreamFilter&&) noexcept = default;

std::string TextToolCallStreamFilter::push(std::string_view chunk) {
    return impl_->push(chunk);
}

TextToolCallRecoveryResult TextToolCallStreamFilter::finish() {
    return impl_->finish();
}

void TextToolCallStreamFilter::reset() { impl_->reset(); }

bool TextToolCallStreamFilter::capturing() const { return impl_->capturing; }

std::size_t TextToolCallStreamFilter::held_bytes() const {
    return impl_->capturing ? impl_->held.size() : 0;
}

std::string TextToolCallStreamFilter::held_excerpt(std::size_t max_bytes) const {
    return truncate_utf8_prefix(impl_->held, max_bytes);
}

std::size_t TextToolCallStreamFilter::debug_bytes_scanned() const {
    return impl_->scanned_total + impl_->parser.scanned();
}

TextToolCallRecoveryResult recover_text_tool_calls(
    std::string_view text,
    const std::vector<ToolDef>& tools) {
    TextToolCallStreamFilter filter(tools);
    std::string visible = filter.push(text);
    auto result = filter.finish();
    result.visible_text = visible + result.visible_text;
    return result;
}

// ---- 可疑级 / 污染检测 -----------------------------------------------------

namespace {

// `<name` 后接至少一个空白再接 `name=`。
bool tag_with_name_attr_at(std::string_view text, std::size_t pos,
                           std::string_view tag) {
    if (!starts_with_at(text, pos, tag)) return false;
    std::size_t i = pos + tag.size();
    const std::size_t ws_start = i;
    while (i < text.size() && is_ws(text[i])) ++i;
    return i > ws_start && starts_with_at(text, i, kNameAttr);
}

// 返回命中的格式;空串 = 不是调用标记。
std::string markup_format_at(std::string_view text, std::size_t pos,
                             bool at_line_prefix) {
    if (at_line_prefix) {
        if (starts_with_at(text, pos, kFunctionCallsOpen)) return "function_calls";
        if (starts_with_at(text, pos, kDotsOpen)) return "dots_function_call";
        if (starts_with_at(text, pos, kToolCallOpen)) return "tool_call_json";
    }
    if (tag_with_name_attr_at(text, pos, kInvokeOpen) ||
        tag_with_name_attr_at(text, pos, kParameterOpen)) {
        return "invoke";
    }
    if (starts_with_at(text, pos, kParameterEqOpen) ||
        starts_with_at(text, pos, kFunctionEqOpen)) {
        return "tool_call_function";
    }
    return {};
}

} // namespace

std::optional<TextToolCallDiagnostic> detect_suspicious_text_tool_call(
    std::string_view visible_text) {
    MarkdownFenceTracker fence;
    std::size_t line_start = 0;
    for (std::size_t i = 0; i < visible_text.size(); ++i) {
        const char c = visible_text[i];
        if (c == '<' && !fence.in_fence() && !fence.line_opening_fence() &&
            !fence.in_inline_code()) {
            std::string format =
                markup_format_at(visible_text, i, fence.at_line_prefix());
            if (!format.empty()) {
                TextToolCallDiagnostic diag;
                diag.outcome = TextToolCallDiagnostic::Outcome::Rejected;
                diag.reason = "malformed";
                diag.format = std::move(format);
                diag.error = "the reply contains tool-call markup that could not be "
                             "parsed as a complete call";
                diag.visible_cut = line_start;
                diag.raw_excerpt = truncate_utf8_prefix(
                    std::string(visible_text.substr(line_start)), kRawExcerptBytes, "");
                return diag;
            }
        }
        fence.feed(c);
        if (c == '\n') line_start = i + 1;
    }
    return std::nullopt;
}

bool text_contains_tool_call_markup(std::string_view text) {
    if (text.find(kDsmlMarker) != std::string_view::npos) return true;
    return detect_suspicious_text_tool_call(text).has_value();
}

// ---- 混合形态:回显比对 -----------------------------------------------------

namespace {

nlohmann::json comparable_arguments(const std::string& raw) {
    if (is_blank(raw)) return nlohmann::json::object();
    auto parsed = nlohmann::json::parse(raw, nullptr, false);
    if (parsed.is_discarded()) return nlohmann::json(raw);
    return parsed;
}

} // namespace

void drop_echoes_of_native_calls(std::vector<ToolCall>& text_calls,
                                 const std::vector<ToolCall>& native,
                                 const std::vector<ToolDef>& tools) {
    if (text_calls.empty() || native.empty()) return;
    const ToolIndex index(tools);
    std::vector<std::pair<std::string, nlohmann::json>> native_keys;
    native_keys.reserve(native.size());
    for (const auto& call : native) {
        native_keys.emplace_back(index.normalized_name(call.function_name),
                                 comparable_arguments(call.function_arguments));
    }
    std::vector<bool> used(native.size(), false);
    std::vector<ToolCall> remaining;
    for (auto& call : text_calls) {
        const std::string name = index.normalized_name(call.function_name);
        const nlohmann::json args = comparable_arguments(call.function_arguments);
        bool echoed = false;
        for (std::size_t j = 0; j < native_keys.size(); ++j) {
            if (used[j]) continue;
            if (native_keys[j].first == name && native_keys[j].second == args) {
                used[j] = true;
                echoed = true;
                break;
            }
        }
        if (!echoed) remaining.push_back(std::move(call));
    }
    text_calls = std::move(remaining);
}

std::string describe_unexecuted_text_tool_call(const ToolCall& call) {
    std::vector<std::string> keys;
    const auto args = comparable_arguments(call.function_arguments);
    if (args.is_object()) {
        for (auto it = args.begin(); it != args.end(); ++it) keys.push_back(it.key());
    }
    return call.function_name + "(" + join(keys, ", ") + ")";
}

TextToolCallDiagnostic diagnose_text_tool_calls_with_native(
    const TextToolCallRecoveryResult& text,
    const std::vector<ToolCall>& native,
    const std::vector<ToolDef>& tools) {
    TextToolCallDiagnostic diag;
    using Outcome = TextToolCallDiagnostic::Outcome;
    if (text.diagnostic.outcome != Outcome::Recovered &&
        text.diagnostic.outcome != Outcome::Rejected) {
        return diag;
    }
    std::vector<ToolCall> remaining = text.candidate_calls;
    drop_echoes_of_native_calls(remaining, native, tools);
    if (remaining.empty()) return diag;
    diag.outcome = Outcome::IgnoredWithNative;
    diag.format = text.diagnostic.format;
    diag.raw_excerpt = text.diagnostic.raw_excerpt;
    diag.error = "tool calls written as text alongside native tool calls were not "
                 "executed";
    for (const auto& call : remaining) {
        diag.attempted_tools.push_back(call.function_name);
        diag.unexecuted_detail.push_back(describe_unexecuted_text_tool_call(call));
    }
    return diag;
}

// ---- 文案 ------------------------------------------------------------------

std::string build_text_tool_call_correction_prompt(
    const TextToolCallDiagnostic& diagnostic,
    const std::vector<std::string>& model_tool_names) {
    std::string problem = diagnostic.error.empty()
                              ? std::string("the tool call could not be parsed")
                              : diagnostic.error;
    if (problem.back() != '.') problem.push_back('.');
    std::string text =
        "[SYSTEM NOTE] Your previous reply wrote a tool call as text inside the "
        "message body instead of using the native tool-calling (function calling) "
        "interface, so nothing was executed. Problem: " +
        problem;
    if (diagnostic.reason == "unknown_tool" && !model_tool_names.empty()) {
        text += "\nAvailable tools: " + join(model_tool_names, ", ", kMaxListedTools) +
                ".";
    }
    if (diagnostic.reason == "truncated_by_length") {
        text += "\nThe reply hit the output length limit while writing the call. "
                "Split large content into several smaller calls (for edits prefer "
                "small targeted changes).";
    }
    text += "\nRe-issue the call now through the native tool-calling interface. "
            "Do not write tool calls as tags or JSON in your reply text. If you did "
            "not intend to call a tool, answer in plain text and put any example "
            "markup inside a fenced code block.";
    return text;
}

std::string build_text_tool_call_ignored_note(
    const TextToolCallDiagnostic& diagnostic) {
    if (diagnostic.unexecuted_detail.empty()) return {};
    return "[SYSTEM NOTE] Your previous reply also wrote these tool call(s) as text "
           "in the message body: " +
           join(diagnostic.unexecuted_detail, ", ") +
           ". They were NOT executed; only your native tool calls ran. If you still "
           "need them, issue them through the native tool-calling interface.";
}

// ---- 历史清洗 ---------------------------------------------------------------

namespace {

bool is_blank_text(std::string_view text) {
    return text.find_first_not_of(" \t\r\n") == std::string_view::npos;
}

std::string rtrim_copy(std::string_view text) {
    const auto last = text.find_last_not_of(" \t\r\n");
    if (last == std::string_view::npos) return {};
    return std::string(text.substr(0, last + 1));
}

// 对一段正文做结构判定(不校验工具名,工具表传空):有调用块、块前只有空白
// 时返回 true,visible_out 为块前的可见正文;块前有正文时同样返回 true,
// visible_out 为那段正文(摘要场景要保留它)。没有调用块返回 false。
bool strip_trailing_text_tool_call(std::string_view text, std::string& visible_out) {
    static const std::vector<ToolDef> kNoTools;
    auto result = recover_text_tool_calls(text, kNoTools);
    if (result.diagnostic.outcome == TextToolCallDiagnostic::Outcome::None) {
        return false;
    }
    visible_out = std::move(result.visible_text);
    return true;
}

} // namespace

void sanitize_text_tool_call_history(std::vector<ChatMessage>& history,
                                     const std::string& summary_prefix) {
    const std::string placeholder = kTextToolCallHistoryPlaceholder;
    const std::string summary_head = summary_prefix + "\n";
    for (auto& msg : history) {
        // 快速路径:没有 '<' 就不可能有调用块。
        if (msg.content.find('<') == std::string::npos) continue;
        const bool has_head = !summary_prefix.empty() &&
            msg.content.rfind(summary_head, 0) == 0;
        const bool is_summary = msg.is_compact_summary || has_head;
        if (is_summary) {
            const std::string head = has_head ? summary_head : std::string{};
            const std::string_view body =
                std::string_view(msg.content).substr(head.size());
            std::string visible;
            if (!strip_trailing_text_tool_call(body, visible)) continue;
            std::string kept = rtrim_copy(visible);
            std::string rebuilt = head;
            if (kept.empty()) {
                rebuilt += placeholder + "\n(summary unavailable)";
            } else {
                rebuilt += kept + "\n\n" + placeholder;
            }
            msg.content = std::move(rebuilt);
            continue;
        }
        if (msg.role != "assistant") continue;
        if (msg.tool_calls.is_array() && !msg.tool_calls.empty()) continue;
        std::string visible;
        if (!strip_trailing_text_tool_call(msg.content, visible)) continue;
        if (!is_blank_text(visible)) continue; // 块前有正文:不是纯文本调用
        msg.content = placeholder;
    }
}

} // namespace acecode
