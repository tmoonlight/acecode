#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <ftxui/dom/elements.hpp>

namespace acecode::tui {

// L1 消息级 Element 渲染缓存键:内容不变且渲染上下文(宽度/主题/语法)
// 不变时,消息的渲染结果可以复用。
struct MessageRenderCacheKey {
    std::size_t revision = 0;
    int width = 0;
    std::uint32_t theme_version = 0;
    bool syntax = true;

    bool operator==(const MessageRenderCacheKey& o) const {
        return revision == o.revision && width == o.width &&
               theme_version == o.theme_version && syntax == o.syntax;
    }
};

// 缓存的链接区域:坐标是相对消息盒子的偏移,命中重放时按当前布局 box 调整。
struct CachedLinkRegion {
    std::string href;
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

// 每消息的 Element 缓存(纯优化):命中则跳过 format_markdown,直接复用。
// 任何缓存失效/异常都回退到全量渲染路径,缓存不承担正确性。
class MessageRenderCache {
public:
    void resize(std::size_t n) {
        valid_.assign(n, false);
        keys_.assign(n, MessageRenderCacheKey{});
        elements_.assign(n, std::nullopt);
        links_.assign(n, {});
    }

    void invalidate_all() {
        std::fill(valid_.begin(), valid_.end(), false);
        for (auto& e : elements_) e.reset();
        for (auto& l : links_) l.clear();
    }

    void invalidate(std::size_t i) {
        if (i < valid_.size()) {
            valid_[i] = false;
            elements_[i].reset();
            links_[i].clear();
        }
    }

    bool valid(std::size_t i, const MessageRenderCacheKey& key) const {
        return i < valid_.size() && valid_[i] && keys_[i] == key &&
               elements_[i].has_value();
    }

    void store(std::size_t i, const MessageRenderCacheKey& key,
               ftxui::Element element, std::vector<CachedLinkRegion> links) {
        if (i >= valid_.size()) return;
        valid_[i] = true;
        keys_[i] = key;
        elements_[i] = std::move(element);
        links_[i] = std::move(links);
    }

    const ftxui::Element* element(std::size_t i) const {
        if (i < elements_.size() && elements_[i].has_value()) {
            return &(*elements_[i]);
        }
        return nullptr;
    }

    const std::vector<CachedLinkRegion>& link_regions(std::size_t i) const {
        static const std::vector<CachedLinkRegion> kEmpty;
        return (i < links_.size()) ? links_[i] : kEmpty;
    }

private:
    std::vector<bool> valid_;
    std::vector<MessageRenderCacheKey> keys_;
    std::vector<std::optional<ftxui::Element>> elements_;
    std::vector<std::vector<CachedLinkRegion>> links_;
};

} // namespace acecode::tui
