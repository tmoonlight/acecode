#pragma once
#include "tui/app/ui_post_target.hpp"
#include <memory>
namespace acecode { class SessionModelBinding; struct AppConfig; class AgentLoop; }
namespace acecode::tui {
class ModelPoolMonitorSubscription {
public:
    ModelPoolMonitorSubscription(SessionModelBinding& binding, AppConfig& config,
        AgentLoop& agent, std::weak_ptr<UiPostTarget> target);
    ~ModelPoolMonitorSubscription();
    void stop();
private:
    void changed();
    SessionModelBinding& binding_;
    AppConfig& config_;
    AgentLoop& agent_;
    std::weak_ptr<UiPostTarget> target_;
    bool stopped_ = false;
    LifetimeToken lifetime_;
};
}
