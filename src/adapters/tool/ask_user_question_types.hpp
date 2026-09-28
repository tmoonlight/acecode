#pragma once

#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace acecode {

// Shared AskUserQuestion domain data. This header intentionally has no UI or
// tool-executor dependencies so controller/layout code stays portable.
struct AskOption {
    std::string label;
    std::string description;
    bool recommended = false;
};

struct AskQuestion {
    std::string question;
    std::string header;
    std::vector<AskOption> options;
    bool multi_select = false;
};

inline bool ask_option_label_has_recommended_suffix(std::string_view label) {
    for (const std::string_view suffix : {"(Recommended)", "[Recommended]"}) {
        if (label.size() < suffix.size()) continue;
        const std::size_t start = label.size() - suffix.size();
        if (label.compare(start, suffix.size(), suffix) == 0 &&
            (start == 0 || std::isspace(static_cast<unsigned char>(label[start - 1])))) {
            return true;
        }
    }
    return false;
}

} // namespace acecode
