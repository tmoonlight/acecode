#pragma once

// 自动模式（试验）单测用的假裁决模型:按脚本依次回答非流式请求,记录收到的请求。
// 脚本用完后回答空正文(解析结果是「无法解析」),这样漏写脚本的用例不会被
// 默默放行。

#include "llm/llm_provider.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace acecode_test {

class ScriptedReviewer : public acecode::LlmProvider {
public:
    struct Reply {
        std::string text;
        bool error = false;
        int status_code = 0;
        int delay_ms = 0;   // 回答之前等待;期间轮询取消标志
    };
    struct Request {
        std::vector<acecode::ChatMessage> messages;
        std::size_t tool_count = 0;
        bool for_compaction = false;
        std::string prompt_cache_key;
    };

    void push(std::string text) { push_reply({std::move(text), false, 0, 0}); }
    void push_error(int status_code) { push_reply({{}, true, status_code, 0}); }
    void push_slow(int delay_ms, std::string text) { push_reply({std::move(text), false, 0, delay_ms}); }
    void push_reply(Reply reply) {
        std::lock_guard<std::mutex> lock(mu_);
        replies_.push_back(std::move(reply));
    }
    void set_tool_free_chat(bool value) { tool_free_chat_.store(value); }

    int calls() const {
        std::lock_guard<std::mutex> lock(mu_);
        return static_cast<int>(requests_.size());
    }
    std::vector<Request> requests() const {
        std::lock_guard<std::mutex> lock(mu_);
        return requests_;
    }
    // 第 index 次请求里 user 消息的正文(记录 + 待裁决的调用 + 阶段指令)。
    std::string user_text(int index) const {
        std::lock_guard<std::mutex> lock(mu_);
        if (index < 0 || static_cast<std::size_t>(index) >= requests_.size()) return {};
        for (const auto& message : requests_[static_cast<std::size_t>(index)].messages) {
            if (message.role == "user") return message.content;
        }
        return {};
    }

    acecode::ChatResponse chat(const std::vector<acecode::ChatMessage>& messages,
                               const std::vector<acecode::ToolDef>& tools) override {
        return answer(messages, tools, nullptr, false, {});
    }
    acecode::ChatResponse chat_cancellable(const std::vector<acecode::ChatMessage>& messages,
                                           const std::vector<acecode::ToolDef>& tools,
                                           const std::atomic<bool>* abort_flag) override {
        return answer(messages, tools, abort_flag, false, {});
    }
    acecode::ChatResponse chat_with_options(const std::vector<acecode::ChatMessage>& messages,
                                            const std::vector<acecode::ToolDef>& tools,
                                            const acecode::ChatRequestOptions& options,
                                            const std::atomic<bool>* abort_flag) override {
        return answer(messages, tools, abort_flag, options.for_compaction, options.prompt_cache_key);
    }
    void chat_stream(const std::vector<acecode::ChatMessage>&, const std::vector<acecode::ToolDef>&,
                     const acecode::StreamCallback& callback, std::atomic<bool>* = nullptr) override {
        acecode::StreamEvent done;
        done.type = acecode::StreamEventType::Done;
        callback(done);
    }
    std::string name() const override { return "reviewer"; }
    bool is_authenticated() override { return true; }
    bool supports_tool_free_chat() const override { return tool_free_chat_.load(); }
    std::string model() const override {
        std::lock_guard<std::mutex> lock(mu_);
        return model_;
    }
    void set_model(const std::string& model) override {
        std::lock_guard<std::mutex> lock(mu_);
        model_ = model;
    }

private:
    acecode::ChatResponse answer(const std::vector<acecode::ChatMessage>& messages,
                                 const std::vector<acecode::ToolDef>& tools,
                                 const std::atomic<bool>* abort_flag, bool for_compaction,
                                 std::string prompt_cache_key) {
        Reply reply;
        {
            std::lock_guard<std::mutex> lock(mu_);
            requests_.push_back({messages, tools.size(), for_compaction, std::move(prompt_cache_key)});
            if (!replies_.empty()) {
                reply = std::move(replies_.front());
                replies_.erase(replies_.begin());
            }
        }
        acecode::ChatResponse response;
        for (int waited = 0; waited < reply.delay_ms; waited += 5) {
            if (abort_flag && abort_flag->load()) {
                response.finish_reason = "error";
                response.provider_error.kind = acecode::ProviderErrorKind::UserCancelled;
                return response;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (reply.error) {
            response.finish_reason = "error";
            response.provider_error.kind = acecode::ProviderErrorKind::Http;
            response.provider_error.status_code = reply.status_code;
            return response;
        }
        response.finish_reason = "stop";
        response.content = std::move(reply.text);
        return response;
    }

    mutable std::mutex mu_;
    std::vector<Reply> replies_;
    std::vector<Request> requests_;
    std::string model_ = "reviewer-1";
    std::atomic<bool> tool_free_chat_{true};
};

} // namespace acecode_test
