#include "side_question_service.hpp"
#include "utils/text.hpp"

#include <algorithm>
#include <utility>

namespace acecode::agent {

struct SideQuestionService::State {
    explicit State(ProviderAccessor accessor) : provider(std::move(accessor)) {}
    ProviderAccessor provider;
    std::mutex context_mu;
    std::vector<ChatMessage> context;
    std::atomic<bool> stopped{false};
    // In-flight requests, so stop_requests can cancel a multi-step tool loop
    // instead of join() waiting for every remaining model call.
    std::mutex requests_mu;
    std::vector<SideChatCancellation*> in_flight;
};

namespace {

// Registers one request's cancellation for its whole run. A request admitted
// after stop_requests is cancelled before it reaches the provider.
template <typename State>
class InFlightRequest {
public:
    InFlightRequest(State& state, SideChatCancellation& cancellation)
        : state_(state), cancellation_(cancellation) {
        std::lock_guard<std::mutex> lock(state_.requests_mu);
        state_.in_flight.push_back(&cancellation_);
        if (state_.stopped.load()) cancellation_.cancel();
    }
    ~InFlightRequest() {
        std::lock_guard<std::mutex> lock(state_.requests_mu);
        auto& requests = state_.in_flight;
        requests.erase(std::remove(requests.begin(), requests.end(), &cancellation_), requests.end());
    }
    InFlightRequest(const InFlightRequest&) = delete;
    InFlightRequest& operator=(const InFlightRequest&) = delete;

private:
    State& state_;
    SideChatCancellation& cancellation_;
};

} // namespace

SideQuestionService::SideQuestionService(ProviderAccessor provider)
    : state_(std::make_shared<State>(std::move(provider))) {}

SideQuestionService::~SideQuestionService() {
    stop_requests();
    join();
}

void SideQuestionService::stop_requests() {
    state_->stopped.store(true);
    std::lock_guard<std::mutex> lock(state_->requests_mu);
    for (auto* cancellation : state_->in_flight) cancellation->cancel();
}

void SideQuestionService::join() {
    callback_lifetime_.revoke();
    threads_.shutdown();
}

void SideQuestionService::publish(const std::vector<ChatMessage>& messages) {
    std::lock_guard<std::mutex> lock(state_->context_mu);
    state_->context = messages;
}

std::vector<ChatMessage> SideQuestionService::snapshot() const {
    std::lock_guard<std::mutex> lock(state_->context_mu);
    return state_->context;
}

SideQuestionResult SideQuestionService::ask(const std::string& question,
                                            const SideChatToolset& tools) {
    return run_question(state_, question, tools);
}

SideQuestionResult SideQuestionService::run_question(
    const std::shared_ptr<State>& state, const std::string& question,
    const SideChatToolset& tools) {
    std::vector<ChatMessage> context;
    {
        std::lock_guard<std::mutex> lock(state->context_mu);
        context = state->context;
    }
    std::shared_ptr<LlmProvider> provider;
    if (state->provider) provider = state->provider();

    SideChatCancellation cancellation;
    InFlightRequest<State> registration(*state, cancellation);
    std::vector<std::string> tools_used;
    auto chat = run_side_chat(std::move(provider), std::move(context), question, {},
                              cancellation, {}, tools, [&tools_used](const SideChatToolEvent& event) {
        if (event.status != "running") return;
        tools_used.push_back(event.target.empty() ? event.name : event.name + " " + event.target);
    });
    SideQuestionResult result = std::move(chat.response);
    // One-turn answers are shown inline after "[/btw] ", so surrounding
    // whitespace from the stream is dropped (streaming side chat keeps it).
    result.answer = utils::trim_ascii_copy(result.answer);
    result.tools_used = std::move(tools_used);
    if (chat.cancelled) {
        // Only shutdown cancels a one-turn question; its callback is suppressed.
        result.status = SideQuestionStatus::Failed;
        result.error = "side question cancelled";
        result.answer.clear();
    }
    return result;
}

SideChatResult SideQuestionService::stream(
    const std::string& question,
    const std::vector<SideChatMessage>& history,
    SideChatCancellation& cancellation,
    const SideChatStreamCallback& callback,
    const SideChatToolset& tools,
    const SideChatToolCallback& on_tool) {
    auto provider = state_->provider ? state_->provider() : nullptr;
    InFlightRequest<State> registration(*state_, cancellation);
    return run_side_chat(std::move(provider), snapshot(), question, history,
                         cancellation, callback, tools, on_tool);
}

bool SideQuestionService::ask_async(std::string question, Callback callback,
                                    SideChatToolset tools) {
    if (state_->stopped.load()) return false;
    // State and the callback are owned by the request. The worker never captures
    // the facade or this service; joining cannot leave borrowed state behind.
    return threads_.spawn([state = state_, ref = callback_lifetime_.ref(*state_), question = std::move(question),
                           callback = std::move(callback), tools = std::move(tools)]() mutable {
        auto result = run_question(state, question, tools);
        ref.with([&](State& active) {
            if (!active.stopped.load() && callback) callback(std::move(result));
        });
    });
}

} // namespace acecode::agent
