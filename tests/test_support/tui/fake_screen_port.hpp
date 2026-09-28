#pragma once
#include "tui/screen_port.hpp"
#include <deque>
#include <mutex>
#include <utility>
#include <vector>

namespace acecode::tui::test_support {
class FakeScreenPort final : public IScreenPort {
public:
    void post_event(ftxui::Event event) override {
        std::lock_guard<std::mutex> lock(mu_);
        events_.push_back(std::move(event));
    }
    void post_task(std::function<void()> task) override {
        std::lock_guard<std::mutex> lock(mu_);
        tasks_.push_back(std::move(task));
    }
    void exit() override {
        std::lock_guard<std::mutex> lock(mu_);
        exited_ = true;
    }
    std::string get_selection() override {
        std::lock_guard<std::mutex> lock(mu_);
        return selection_;
    }
    void shift_selection(int dx, int dy) override {
        std::lock_guard<std::mutex> lock(mu_);
        shifts_.emplace_back(dx, dy);
    }
    int dimx() const override {
        std::lock_guard<std::mutex> lock(mu_);
        return width_;
    }
    void set_width(int width) { std::lock_guard<std::mutex> lock(mu_); width_ = width; }
    void set_selection(std::string text) {
        std::lock_guard<std::mutex> lock(mu_);
        selection_ = std::move(text);
    }
    bool exited() const { std::lock_guard<std::mutex> lock(mu_); return exited_; }
    std::vector<ftxui::Event> take_events() {
        std::lock_guard<std::mutex> lock(mu_);
        auto events = std::move(events_);
        events_.clear();
        return events;
    }
    std::vector<std::pair<int, int>> shifts() const {
        std::lock_guard<std::mutex> lock(mu_); return shifts_;
    }
    bool run_next_task() {
        std::function<void()> task;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (tasks_.empty()) return false;
            task = std::move(tasks_.front());
            tasks_.pop_front();
        }
        if (task) task();
        return true;
    }
private:
    mutable std::mutex mu_;
    int width_ = 80;
    bool exited_ = false;
    std::string selection_;
    std::vector<ftxui::Event> events_;
    std::vector<std::pair<int, int>> shifts_;
    std::deque<std::function<void()>> tasks_;
};
} // namespace acecode::tui::test_support
