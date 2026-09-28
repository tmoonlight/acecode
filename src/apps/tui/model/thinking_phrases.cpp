#include "tui/model/thinking_phrases.hpp"
#include <random>

namespace acecode::tui {
static const std::string EN_THINKING_PHRASES[50] = {
    "Analyzing", "Pondering", "Investigating", "Synthesizing", "Reviewing",
    "Processing", "Compiling", "Evaluating", "Formulating", "Brainstorming",
    "Searching", "Deciphering", "Gathering", "Debugging", "Inspecting",
    "Generating", "Organizing", "Mapping", "Exploring", "Tracing",
    "Validating", "Considering", "Reflecting", "Simulating", "Calculating",
    "Abstracting", "Diving", "Looking", "Troubleshooting", "Crafting",
    "Polishing", "Assembling", "Connecting", "Building", "Parsing",
    "Extracting", "Tuning", "Optimizing", "Designing", "Theorizing",
    "Hypothesizing", "Seeking", "Interpreting", "Measuring", "Weighing",
    "Reading", "Preparing", "Reasoning", "Constructing", "Finalizing"
};

static const std::string ZH_THINKING_PHRASES[50] = {
    "分析中", "思考中", "研究中", "探索中", "综合中",
    "审查中", "处理中", "编译中", "评估中", "规划中",
    "构思中", "搜索中", "解码中", "收集中", "调试中",
    "检查中", "生成中", "组织中", "映射中", "推理中",
    "验证中", "考虑中", "反思中", "模拟中", "计算中",
    "抽象中", "深挖中", "寻找中", "排查中", "打磨中",
    "完善中", "组装中", "连接中", "构建中", "解析中",
    "提取中", "微调中", "优化中", "设计中", "推论中",
    "假设中", "路线中", "解读中", "测量中", "权衡中",
    "阅读中", "准备中", "追溯中", "构造中", "总结中"
};

bool is_user_chinese(const TuiState& state) {
    if (state.conversation.empty()) return false;
    for (auto it = state.conversation.rbegin(); it != state.conversation.rend(); ++it) {
        if (it->role == "user") {
            for (unsigned char c : it->content) {
                if (c >= 0xE0) return true;
            }
            return false;
        }
    }
    return false;
}

std::string get_random_thinking_phrase(bool is_zh) {
    static thread_local std::random_device rd;
    static thread_local std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 49);
    return is_zh ? ZH_THINKING_PHRASES[dis(gen)] : EN_THINKING_PHRASES[dis(gen)];
}


} // namespace acecode::tui
