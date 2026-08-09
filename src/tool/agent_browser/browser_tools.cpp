#include "browser_tools.hpp"

#include "cdp_client.hpp"

#include "desktop/agent_browser_runtime.hpp"
#include "utils/logger.hpp"
#include "utils/utf8_path.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <thread>

namespace acecode::agent_browser {
namespace {

using json = nlohmann::json;

constexpr auto kConnectTimeout = std::chrono::seconds(10);
constexpr auto kCommandTimeout = std::chrono::seconds(15);
constexpr std::uint32_t kMaxSnapshotRevision = 1'000'000'000u;

std::atomic<std::uint32_t> snapshot_revision_seed{
    static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count() %
        900'000'000)};

std::uint32_t next_snapshot_revision() {
    return 1u + (snapshot_revision_seed.fetch_add(
                     1u, std::memory_order_relaxed) %
                 kMaxSnapshotRevision);
}

json object_schema(json properties, json required = json::array()) {
    properties["page_id"] = {
        {"type", "string"},
        {"description", "Optional Browser page id returned by browser_open. Omit to use the active page at tool start."},
    };
    return {
        {"type", "object"},
        {"properties", std::move(properties)},
        {"required", std::move(required)},
        {"additionalProperties", false},
    };
}

json string_property(const std::string& description) {
    return {{"type", "string"}, {"description", description}};
}

json integer_property(const std::string& description,
                      int minimum = 0,
                      int maximum = 120000) {
    return {
        {"type", "integer"},
        {"description", description},
        {"minimum", minimum},
        {"maximum", maximum},
    };
}

void add_macos_input_mode_property(json& properties) {
#ifdef __APPLE__
    properties["input_mode"] = {
        {"type", "string"},
        {"enum", {"synthetic", "native"}},
        {"default", "synthetic"},
        {"description",
         "macOS input backend. Start with synthetic (default, no permission). Choose native when a page rejects untrusted DOM events or needs real pointer/keyboard gestures; native requires macOS Accessibility permission."},
    };
#else
    (void)properties;
#endif
}

const char* interaction_description(const char* existing,
                                    const char* macos) {
#ifdef __APPLE__
    (void)existing;
    return macos;
#else
    (void)macos;
    return existing;
#endif
}

std::string selected_input_mode(const json& args) {
#ifdef __APPLE__
    return args.value("input_mode", "synthetic");
#else
    (void)args;
    return {};
#endif
}

void apply_input_mode(json& params, const json& args) {
#ifdef __APPLE__
    params["acecodeInputMode"] = selected_input_mode(args);
#else
    (void)params;
    (void)args;
#endif
}

json with_input_metadata(json value, const json& args) {
    return agent_browser_input_result_metadata(
        std::move(value), selected_input_mode(args));
}

bool parse_arguments(const std::string& text, json& args, std::string& error) {
    args = json::parse(text.empty() ? "{}" : text, nullptr, false);
    if (!args.is_object()) {
        error = "arguments must be a JSON object";
        return false;
    }
    return true;
}

ToolResult result_json(json value,
                       bool success,
                       const std::string& verb,
                       const std::string& object,
                       std::vector<std::pair<std::string, std::string>> metrics = {},
                       const std::string& page_id = {}) {
    ToolResult result;
    result.output = value.dump(2);
    result.success = success;
    result.summary = ToolSummary{verb, object, std::move(metrics), "world"};
    result.metadata = {{"agent_browser", true}};
    if (!page_id.empty()) result.metadata["page_id"] = page_id;
    return result;
}

ToolResult failure_result(const std::string& code,
                          const std::string& message,
                          const std::string& object = "Browser") {
    return result_json(
        {{"ok", false}, {"error", {{"code", code}, {"message", message}}}},
        false,
        "Browser",
        object);
}

ToolResult input_failure_result(const std::string& fallback_code,
                                const std::string& error,
                                const std::string& object = "Browser") {
    if (error.rfind("native_input_permission_required", 0) == 0) {
        return failure_result(
            "native_input_permission_required", error, object);
    }
    if (error.rfind("native_input_requires_visible_active_page", 0) == 0) {
        return failure_result(
            "native_input_requires_visible_active_page", error, object);
    }
    return failure_result(fallback_code, error, object);
}

ToolResult success_result(json data,
                          const std::string& verb,
                          const std::string& object,
                          std::vector<std::pair<std::string, std::string>> metrics = {},
                          const std::string& page_id = {}) {
    if (!data.is_object()) data = {{"result", std::move(data)}};
    data["ok"] = true;
    if (!page_id.empty()) data["page_id"] = page_id;
    return result_json(std::move(data), true, verb, object,
                       std::move(metrics), page_id);
}

bool connect_client(AgentBrowserCdpClient& client,
                     const ToolContext& context,
                     std::string& error,
                     const json* args = nullptr,
                     bool claim_page = true) {
    if (!client.connect(kConnectTimeout, context.abort_flag, error)) return false;
    const std::string requested_page = args && args->is_object()
        ? args->value("page_id", "") : std::string();
    if (!requested_page.empty()) {
        return client.select_page(requested_page, kCommandTimeout,
                                  context.abort_flag, error);
    }
    return !claim_page || client.claim_page(
        kCommandTimeout, context.abort_flag, error);
}

bool has_target_descriptor(const json& args);

json evaluate(AgentBrowserCdpClient& client,
              const std::string& expression,
              const ToolContext& context,
              std::string& error) {
    const json response = client.command(
        "Runtime.evaluate",
        {
            {"expression", expression},
            {"awaitPromise", true},
            {"returnByValue", true},
            {"userGesture", true},
        },
        kCommandTimeout,
        context.abort_flag,
        error);
    if (!error.empty()) return {};
    if (response.contains("exceptionDetails")) {
        error = response["exceptionDetails"].dump();
        return {};
    }
    return response.value("result", json::object()).value("value", json(nullptr));
}

json page_summary(AgentBrowserCdpClient& client,
                  const ToolContext& context,
                  std::string& error) {
    json value = evaluate(
        client,
        "(() => ({url:location.href,title:document.title,ready_state:document.readyState}))()",
        context,
        error);
    return value.is_object() ? value : json::object();
}

ToolResult success_result_with_page(
    AgentBrowserCdpClient& client,
    const ToolContext& context,
    json data,
    const std::string& verb,
    const std::string& object,
    std::vector<std::pair<std::string, std::string>> metrics = {}) {
    if (!data.is_object()) data = {{"result", std::move(data)}};
    std::string page_error;
    const json current = page_summary(client, context, page_error);
    if (page_error.empty() && current.is_object()) {
        for (const char* key : {"url", "title", "ready_state"}) {
            if (current.contains(key)) data[key] = current[key];
        }
    }
    return success_result(
        std::move(data), verb, object, std::move(metrics), client.page_id());
}

std::string target_resolution_script(const json& args, bool scroll_into_view = false) {
    json payload = args;
    payload["__scrollIntoView"] = scroll_into_view;
    return "(() => {\n"
           " const a=" + payload.dump() + ";\n"
           " const visible=(el)=>{if(!el||!el.isConnected)return false;const r=el.getBoundingClientRect();const s=getComputedStyle(el);return r.width>0&&r.height>0&&s.display!=='none'&&s.visibility!=='hidden'&&s.opacity!=='0';};\n"
           " const text=(el)=>String(el.innerText||el.value||el.getAttribute('aria-label')||el.getAttribute('title')||'').trim();\n"
           " const role=(el)=>el.getAttribute('role')||({A:'link',BUTTON:'button',INPUT:(el.type==='checkbox'?'checkbox':el.type==='radio'?'radio':'textbox'),TEXTAREA:'textbox',SELECT:'combobox',SUMMARY:'button'}[el.tagName]||el.tagName.toLowerCase());\n"
           " let el=null;const ref=String(a.target||'');\n"
           " if(ref&&!/^@e\\d+$/.test(ref))return {ok:false,code:'invalid_reference',message:'Element target must be an @e reference from browser_read_page'};\n"
           " if(/^@e\\d+$/.test(ref)){const s=window.__aceAgentBrowserSnapshot;if(!s)return {ok:false,code:'snapshot_required',message:'Call browser_read_page before using @e refs'};if(!Number.isInteger(Number(a.revision))||Number(a.revision)<=0)return {ok:false,code:'snapshot_revision_required',message:'Pass the revision returned by browser_read_page with every @e reference'};if(Number(a.revision)!==s.revision)return {ok:false,code:'stale_reference',message:'The @e reference belongs to an older page snapshot',current_revision:s.revision};el=s.nodes[Number(ref.slice(2))-1];}\n"
           " else if(a.selector){try{el=document.querySelector(a.selector);}catch(e){return {ok:false,code:'invalid_selector',message:String(e.message||e)};}}\n"
           " else {const wantedRole=String(a.role||'').toLowerCase();const wantedName=String(a.name||'').trim().toLowerCase();const candidates=Array.from(document.querySelectorAll('a,button,input,textarea,select,summary,[role],[onclick],[tabindex]:not([tabindex=\"-1\"])')).filter(visible).filter(n=>(!wantedRole||role(n).toLowerCase()===wantedRole)&&(!wantedName||text(n).toLowerCase().includes(wantedName)));el=candidates[Math.max(0,Number(a.nth||0))]||null;}\n"
           " if(!el||!el.isConnected)return {ok:false,code:'target_not_found',message:'Could not resolve the requested page element'};if(a.__scrollIntoView)el.scrollIntoView({block:'center',inline:'center'});\n"
           " const r=el.getBoundingClientRect();return {ok:true,visible:visible(el),rect:{x:r.x,y:r.y,width:r.width,height:r.height,cx:r.x+r.width/2,cy:r.y+r.height/2},role:role(el),name:text(el).slice(0,200),tag:el.tagName.toLowerCase()};\n"
           "})()";
}

json resolve_target(AgentBrowserCdpClient& client,
                    const json& args,
                     const ToolContext& context,
                     std::string& error) {
    if (!has_target_descriptor(args)) {
        error = "browser target requires target, selector, role, or name";
        return {{"ok", false},
                {"code", "target_required"},
                {"message", error}};
    }
    json resolved = evaluate(client, target_resolution_script(args, true), context, error);
    if (!error.empty()) return {};
    if (!resolved.is_object() || !resolved.value("ok", false)) {
        error = resolved.value("message", "could not resolve browser target");
        return resolved;
    }
    if (!resolved.value("visible", false)) {
        error = "the requested browser target is not visible";
        return resolved;
    }
    return resolved;
}

std::string element_mutation_script(const json& args,
                                    const std::string& action) {
    return "(() => {\n"
           " const a=" + args.dump() + ";const action=" + json(action).dump() + ";\n"
           " const visible=(el)=>{if(!el||!el.isConnected)return false;const r=el.getBoundingClientRect();const s=getComputedStyle(el);return r.width>0&&r.height>0&&s.display!=='none'&&s.visibility!=='hidden';};\n"
           " const text=(el)=>String(el.innerText||el.value||el.getAttribute('aria-label')||el.getAttribute('title')||'').trim();\n"
           " const role=(el)=>el.getAttribute('role')||({A:'link',BUTTON:'button',INPUT:'textbox',TEXTAREA:'textbox',SELECT:'combobox'}[el.tagName]||el.tagName.toLowerCase());\n"
           " let el=null;const ref=String(a.target||'');if(ref&&!/^@e\\d+$/.test(ref))return {ok:false,message:'Element target must be an @e reference from browser_read_page'};if(/^@e\\d+$/.test(ref)){const s=window.__aceAgentBrowserSnapshot;if(!s)return {ok:false,message:'Call browser_read_page before using @e refs'};if(!Number.isInteger(Number(a.revision))||Number(a.revision)<=0)return {ok:false,message:'Pass the revision returned by browser_read_page with every @e reference'};if(Number(a.revision)!==s.revision)return {ok:false,message:'Stale @e reference; call browser_read_page again'};el=s.nodes[Number(ref.slice(2))-1];}else if(a.selector){try{el=document.querySelector(a.selector)}catch(e){return {ok:false,message:String(e.message||e)}}}else{const wr=String(a.role||'').toLowerCase(),wn=String(a.name||'').toLowerCase();el=Array.from(document.querySelectorAll('a,button,input,textarea,select,summary,[role],[onclick],[tabindex]:not([tabindex=\"-1\"])')).filter(visible).filter(n=>(!wr||role(n).toLowerCase()===wr)&&(!wn||text(n).toLowerCase().includes(wn)))[Math.max(0,Number(a.nth||0))]||null;}\n"
           " if(!el||!el.isConnected)return {ok:false,message:'Could not resolve the requested page element'};el.scrollIntoView({block:'center',inline:'center'});if(!visible(el))return {ok:false,message:'The requested page element is not visible'};el.focus();\n"
           " if(action==='fill'){const value=String(a.value==null?'':a.value);const proto=el instanceof HTMLTextAreaElement?HTMLTextAreaElement.prototype:el instanceof HTMLInputElement?HTMLInputElement.prototype:null;const setter=proto&&Object.getOwnPropertyDescriptor(proto,'value')?.set;if(setter)setter.call(el,value);else if('value'in el)el.value=value;else el.textContent=value;el.dispatchEvent(new InputEvent('input',{bubbles:true,inputType:'insertText',data:value}));el.dispatchEvent(new Event('change',{bubbles:true}));return {ok:true,value,tag:el.tagName.toLowerCase()};}\n"
           " return {ok:true,tag:el.tagName.toLowerCase()};\n"
           "})()";
}

bool dispatch_mouse(AgentBrowserCdpClient& client,
                    const std::string& type,
                    double x,
                    double y,
                    const json& extra,
                    const json& action_args,
                    const ToolContext& context,
                    std::string& error) {
    json params{{"type", type}, {"x", x}, {"y", y}};
    for (const auto& item : extra.items()) params[item.key()] = item.value();
    apply_input_mode(params, action_args);
    client.command("Input.dispatchMouseEvent", params, kCommandTimeout,
                   context.abort_flag, error);
    return error.empty();
}

ToolImpl make_tool(const std::string& name,
                   const std::string& description,
                   json parameters,
                   bool read_only,
                   std::function<ToolResult(const json&, const ToolContext&)> execute) {
    ToolImpl tool;
    tool.definition.name = name;
    tool.definition.description = description;
    tool.definition.parameters = std::move(parameters);
    tool.is_read_only = read_only;
    tool.execute = [name, execute = std::move(execute)](
                       const std::string& arguments,
                       const ToolContext& context) {
        json args;
        std::string error;
        if (!parse_arguments(arguments, args, error)) {
            return failure_result("invalid_arguments", error, name);
        }
#ifdef __APPLE__
        if (args.contains("input_mode") &&
            (!args["input_mode"].is_string() ||
             (args["input_mode"] != "synthetic" &&
              args["input_mode"] != "native"))) {
            return failure_result(
                "invalid_arguments",
                "macOS input_mode must be synthetic or native", name);
        }
#endif
        LOG_INFO("[agent-browser-tool] " + name + " start");
        ToolResult result;
        try {
            result = execute(args, context);
        } catch (const std::exception& exception) {
            LOG_ERROR("[agent-browser-tool] " + name +
                      " failed with exception: " + exception.what());
            result = failure_result(
                "browser_tool_failed",
                std::string("Agent Browser tool failed: ") + exception.what(),
                name);
        } catch (...) {
            LOG_ERROR("[agent-browser-tool] " + name +
                      " failed with an unknown exception");
            result = failure_result(
                "browser_tool_failed",
                "Agent Browser tool failed unexpectedly",
                name);
        }
        LOG_INFO("[agent-browser-tool] " + name +
                 (result.success ? " finish ok" : " finish failed"));
        return result;
    };
    return tool;
}

json target_properties() {
    return {
        {"target", string_property("Element reference from browser_read_page, such as @e7.")},
        {"revision", integer_property("Snapshot revision returned by browser_read_page.", 1, 1000000000)},
        {"selector", string_property("CSS selector fallback.")},
        {"role", string_property("Accessible role fallback, such as button or textbox.")},
        {"name", string_property("Accessible name/text fallback.")},
        {"nth", integer_property("Zero-based match index for role/name targeting.", 0, 1000)},
    };
}

bool has_target_descriptor(const json& args) {
    for (const char* key : {"target", "selector", "role", "name"}) {
        if (args.contains(key) && args[key].is_string() &&
            !args[key].get_ref<const std::string&>().empty()) {
            return true;
        }
    }
    return false;
}

void merge_properties(json& destination, const json& source) {
    for (const auto& item : source.items()) destination[item.key()] = item.value();
}

ToolImpl open_tool() {
    return make_tool(
        "browser_open",
        "Create a new visible ACECode Desktop Browser tab. Optionally navigate that new page to an HTTP(S) URL, file URL, or absolute local path. Reuse the returned page_id for precise later browser tools.",
        object_schema({
            {"url", string_property("Optional HTTP(S) URL, file URL, or absolute POSIX/Windows path. Bare hostnames are promoted to HTTPS.")},
        }),
        false,
        [](const json& args, const ToolContext& context) {
            AgentBrowserCdpClient client;
            std::string error;
            if (!connect_client(client, context, error, nullptr, false)) {
                return failure_result("desktop_browser_unavailable", error);
            }
            if (!client.create_page(kCommandTimeout, context.abort_flag, error)) {
                return failure_result("browser_open_failed", error);
            }
            const std::string requested_url = args.value("url", "");
            std::string url = requested_url;
            if (!requested_url.empty()) {
                auto normalized = acecode::desktop::normalize_agent_browser_url(
                    requested_url, &error);
                if (!normalized) {
                    return failure_result("unsafe_url", error, requested_url);
                }
                url = *normalized;
                client.command("Page.navigate", {{"url", url}}, kCommandTimeout,
                               context.abort_flag, error);
                if (!error.empty()) return failure_result("navigation_failed", error, url);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(url.empty() ? 0 : 250));
            json summary = page_summary(client, context, error);
            if (!error.empty()) return failure_result("browser_failed", error);
            return success_result(std::move(summary), "Opened",
                                  url.empty() ? "Browser" : url, {},
                                  client.page_id());
        });
}

ToolImpl navigate_tool() {
    return make_tool(
        "browser_navigate",
        "Navigate the Browser page locked at tool start, or move through that page's history. Pass page_id for an exact tab and use browser_read_page after navigation.",
        object_schema({
            {"url", string_property("HTTP(S) URL, file URL, or absolute POSIX/Windows path for action=goto.")},
            {"action", {{"type", "string"}, {"enum", {"goto", "back", "forward", "reload"}}}},
        }),
        false,
        [](const json& args, const ToolContext& context) {
            AgentBrowserCdpClient client;
            std::string error;
            if (!connect_client(client, context, error, &args)) {
                return failure_result("desktop_browser_unavailable", error);
            }
            const std::string requested_url = args.value("url", "");
            const std::string action = args.value("action", requested_url.empty() ? "reload" : "goto");
            if (action == "goto") {
                if (requested_url.empty()) {
                    return failure_result("invalid_arguments", "browser_navigate goto requires url");
                }
                auto normalized = acecode::desktop::normalize_agent_browser_url(
                    requested_url, &error);
                if (!normalized) {
                    return failure_result("unsafe_url", error, requested_url);
                }
                client.command("Page.navigate", {{"url", *normalized}}, kCommandTimeout,
                               context.abort_flag, error);
            } else if (action == "reload") {
                client.command("Page.reload", json::object(), kCommandTimeout,
                               context.abort_flag, error);
            } else if (action == "back" || action == "forward") {
                const json history = client.command(
                    "Page.getNavigationHistory", json::object(),
                    kCommandTimeout, context.abort_flag, error);
                if (error.empty()) {
                    const json entries = history.value(
                        "entries", json::array());
                    const int current_index = history.value("currentIndex", -1);
                    const int target_index = current_index +
                        (action == "back" ? -1 : 1);
                    if (!entries.is_array() || target_index < 0 ||
                        target_index >= static_cast<int>(entries.size()) ||
                        !entries[static_cast<std::size_t>(target_index)].is_object()) {
                        return failure_result(
                            "navigation_unavailable",
                            "Browser history has no " + action + " entry",
                            action);
                    }
                    const int entry_id = entries[static_cast<std::size_t>(target_index)]
                                             .value("id", 0);
                    if (entry_id <= 0) {
                        return failure_result(
                            "navigation_failed",
                            "Browser history returned an invalid entry",
                            action);
                    }
                    client.command(
                        "Page.navigateToHistoryEntry", {{"entryId", entry_id}},
                        kCommandTimeout, context.abort_flag, error);
                }
            } else {
                return failure_result("invalid_arguments", "unsupported navigation action: " + action);
            }
            if (!error.empty()) return failure_result("navigation_failed", error, action);
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            json summary = page_summary(client, context, error);
            if (!error.empty()) return failure_result("browser_failed", error);
            return success_result(std::move(summary), "Navigated", action, {},
                                  client.page_id());
        });
}

ToolImpl read_page_tool() {
    return make_tool(
        "browser_read_page",
        "Read the visible Browser page as semantic text plus actionable elements. Returns a revision and @eN refs; pass both to interaction tools. Call again after navigation or major DOM changes.",
        object_schema({
            {"max_elements", integer_property("Maximum actionable elements to return.", 1, 500)},
            {"max_text_chars", integer_property("Maximum body-text characters to return.", 100, 50000)},
        }),
        true,
        [](const json& args, const ToolContext& context) {
            AgentBrowserCdpClient client;
            std::string error;
            if (!connect_client(client, context, error, &args)) {
                return failure_result("desktop_browser_unavailable", error);
            }
            const int max_elements = args.value("max_elements", 300);
            const int max_text = args.value("max_text_chars", 30000);
            const json options{
                {"maxElements", max_elements},
                {"maxText", max_text},
                {"revision", next_snapshot_revision()},
            };
            const std::string script = "(" + agent_browser_snapshot_script() +
                                       ")(" + options.dump() + ")";
            json value = evaluate(client, script, context, error);
            if (!error.empty()) return failure_result("page_read_failed", error);
            if (!value.is_object()) return failure_result("page_read_failed", "page snapshot was not an object");
            const auto count = value.value("elements", json::array()).size();
            return success_result(
                std::move(value),
                "Read",
                "Browser page",
                {{"elements", std::to_string(count)}},
                client.page_id());
        });
}

ToolImpl click_tool() {
    json properties = target_properties();
    merge_properties(properties, {
        {"button", {{"type", "string"}, {"enum", {"left", "middle", "right"}}}},
        {"click_count", integer_property("Click count.", 1, 3)},
    });
    add_macos_input_mode_property(properties);
    return make_tool(
        "browser_click",
        interaction_description(
            "Click an element in the visible Browser using a real CDP pointer event. Prefer @eN plus revision from browser_read_page.",
            "Click an element in the visible Browser. Prefer @eN plus revision from browser_read_page. On macOS start with synthetic input and choose native only when the page rejects untrusted events or needs a real OS pointer gesture."),
        object_schema(std::move(properties)),
        false,
        [](const json& args, const ToolContext& context) {
            if (!has_target_descriptor(args)) {
                return failure_result(
                    "invalid_arguments",
                    "browser_click requires target, selector, role, or name");
            }
            AgentBrowserCdpClient client;
            std::string error;
            if (!connect_client(client, context, error, &args)) return failure_result("desktop_browser_unavailable", error);
            json target = resolve_target(client, args, context, error);
            if (!error.empty()) return failure_result("target_not_found", error);
            const auto& rect = target["rect"];
            const double x = rect.value("cx", 0.0);
            const double y = rect.value("cy", 0.0);
            const std::string button = args.value("button", "left");
            const int count = args.value("click_count", 1);
            if (!dispatch_mouse(client, "mouseMoved", x, y, json::object(), args, context, error) ||
                !dispatch_mouse(client, "mousePressed", x, y,
                                {{"button", button}, {"clickCount", count}}, args, context, error) ||
                !dispatch_mouse(client, "mouseReleased", x, y,
                                {{"button", button}, {"clickCount", count}}, args, context, error)) {
                return input_failure_result("click_failed", error);
            }
            return success_result_with_page(
                client,
                context,
                with_input_metadata(
                    {{"target", target}, {"clicked", true}}, args),
                "Clicked",
                target.value("name", args.value("target", "element")));
        });
}

ToolImpl fill_tool() {
    json properties = target_properties();
    properties["value"] = string_property("Replacement field value.");
    add_macos_input_mode_property(properties);
    return make_tool(
        "browser_fill",
        interaction_description(
            "Replace an input, textarea, select-like, or editable element's value and dispatch input/change events.",
            "Replace an input, textarea, or editable element's value. On macOS synthetic uses the native DOM value setter and input/change events; choose native when the page requires real Cmd+A and keyboard input."),
        object_schema(std::move(properties), {"value"}),
        false,
        [](const json& args, const ToolContext& context) {
            if (!has_target_descriptor(args)) {
                return failure_result(
                    "invalid_arguments",
                    "browser_fill requires target, selector, role, or name");
            }
            AgentBrowserCdpClient client;
            std::string error;
            if (!connect_client(client, context, error, &args)) return failure_result("desktop_browser_unavailable", error);
            json value;
#ifdef __APPLE__
            if (selected_input_mode(args) == "native") {
                value = evaluate(
                    client, element_mutation_script(args, "focus"),
                    context, error);
                if (error.empty() && value.value("ok", false)) {
                    json key_down{{"type", "keyDown"}, {"key", "a"},
                                  {"code", "KeyA"}, {"modifiers", 4}};
                    apply_input_mode(key_down, args);
                    client.command("Input.dispatchKeyEvent", key_down,
                                   kCommandTimeout, context.abort_flag, error);
                    if (error.empty()) {
                        key_down["type"] = "keyUp";
                        client.command("Input.dispatchKeyEvent", key_down,
                                       kCommandTimeout, context.abort_flag, error);
                    }
                    if (error.empty()) {
                        json text_params{{"text", args.value("value", "")}};
                        apply_input_mode(text_params, args);
                        client.command("Input.insertText", text_params,
                                       kCommandTimeout, context.abort_flag, error);
                    }
                    value = {{"ok", error.empty()},
                             {"value", args.value("value", "")},
                             {"tag", value.value("tag", "")}};
                }
            } else
#endif
            {
                value = evaluate(
                    client, element_mutation_script(args, "fill"),
                    context, error);
            }
            if (!error.empty() || !value.value("ok", false)) {
                return input_failure_result(
                    "fill_failed",
                    error.empty() ? value.value("message", "fill failed") : error);
            }
            return success_result_with_page(
                client, context, with_input_metadata(value, args), "Filled",
                args.value("target", args.value("name", "field")));
        });
}

ToolImpl type_tool() {
    json properties = target_properties();
    properties["text"] = string_property("Text to type at the current caret.");
    properties["submit"] = {{"type", "boolean"}, {"description", "Press Enter after typing."}};
    add_macos_input_mode_property(properties);
    return make_tool(
        "browser_type",
        interaction_description(
            "Focus a target and type text through CDP input. Unlike browser_fill this preserves the current value/caret. Optionally press Enter.",
            "Focus a target and type text at the current caret. Optionally press Enter. On macOS choose native for sites that require trusted keyboard events; otherwise use synthetic."),
        object_schema(std::move(properties), {"text"}),
        false,
        [](const json& args, const ToolContext& context) {
            if (!has_target_descriptor(args)) {
                return failure_result(
                    "invalid_arguments",
                    "browser_type requires target, selector, role, or name");
            }
            AgentBrowserCdpClient client;
            std::string error;
            if (!connect_client(client, context, error, &args)) return failure_result("desktop_browser_unavailable", error);
            json focused = evaluate(client, element_mutation_script(args, "focus"), context, error);
            if (!error.empty() || !focused.value("ok", false)) {
                return failure_result("type_failed", error.empty() ? focused.value("message", "focus failed") : error);
            }
            json text_params{{"text", args.value("text", "")}};
            apply_input_mode(text_params, args);
            client.command("Input.insertText", text_params,
                           kCommandTimeout, context.abort_flag, error);
            if (error.empty() && args.value("submit", false)) {
                json enter{{"type", "keyDown"}, {"key", "Enter"},
                           {"code", "Enter"}, {"windowsVirtualKeyCode", 13}};
                apply_input_mode(enter, args);
                client.command("Input.dispatchKeyEvent", enter,
                               kCommandTimeout, context.abort_flag, error);
                if (error.empty()) {
                    enter["type"] = "keyUp";
                    client.command("Input.dispatchKeyEvent", enter,
                                   kCommandTimeout, context.abort_flag, error);
                }
            }
            if (!error.empty()) return input_failure_result("type_failed", error);
            return success_result_with_page(
                client, context,
                with_input_metadata(
                    {{"typed", true},
                     {"characters", args.value("text", "").size()}}, args),
                "Typed", args.value("target", args.value("name", "field")));
        });
}

int key_modifiers(const std::string& key) {
    int modifiers = 0;
    std::string part;
    std::istringstream stream(key);
    while (std::getline(stream, part, '+')) {
        std::transform(part.begin(), part.end(), part.begin(), [](char ch) {
            return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        });
        if (part == "alt") modifiers |= 1;
        else if (part == "ctrl" || part == "control") modifiers |= 2;
        else if (part == "meta" || part == "cmd" || part == "command") modifiers |= 4;
        else if (part == "shift") modifiers |= 8;
    }
    return modifiers;
}

std::string final_key_part(const std::string& key) {
    const auto plus = key.find_last_of('+');
    return plus == std::string::npos ? key : key.substr(plus + 1);
}

ToolImpl press_tool() {
    json properties = target_properties();
    properties["key"] = string_property("Key or chord, e.g. Enter, Escape, Tab, Ctrl+A, Shift+Tab.");
    add_macos_input_mode_property(properties);
    return make_tool(
        "browser_press",
        interaction_description(
            "Focus an optional page target and press a keyboard key or modifier chord through CDP.",
            "Focus an optional page target and press a keyboard key or modifier chord. On macOS synthetic is the default; choose native for trusted shortcuts and browser-level key handling."),
        object_schema(std::move(properties), {"key"}),
        false,
        [](const json& args, const ToolContext& context) {
            AgentBrowserCdpClient client;
            std::string error;
            if (!connect_client(client, context, error, &args)) return failure_result("desktop_browser_unavailable", error);
            if (has_target_descriptor(args)) {
                json focused = evaluate(client, element_mutation_script(args, "focus"), context, error);
                if (!error.empty() || !focused.value("ok", false)) {
                    return failure_result("press_failed", error.empty() ? focused.value("message", "focus failed") : error);
                }
            }
            const std::string chord = args.value("key", "");
            const std::string key = final_key_part(chord);
            if (key.empty()) return failure_result("invalid_arguments", "browser_press requires key");
            const int modifiers = key_modifiers(chord);
            const int virtual_key = key.size() == 1
                ? static_cast<unsigned char>(std::toupper(static_cast<unsigned char>(key[0])))
                : (key == "Enter" ? 13 : key == "Tab" ? 9 : key == "Escape" ? 27 : 0);
            json params{{"key", key}, {"code", key}, {"modifiers", modifiers}};
            if (virtual_key) params["windowsVirtualKeyCode"] = virtual_key;
            apply_input_mode(params, args);
            params["type"] = "keyDown";
            client.command("Input.dispatchKeyEvent", params, kCommandTimeout, context.abort_flag, error);
            if (error.empty()) {
                params["type"] = "keyUp";
                client.command("Input.dispatchKeyEvent", params, kCommandTimeout, context.abort_flag, error);
            }
            if (!error.empty()) return input_failure_result("press_failed", error);
            return success_result_with_page(
                client, context,
                with_input_metadata({{"pressed", chord}}, args),
                "Pressed", chord);
        });
}

ToolImpl hover_tool() {
    json properties = target_properties();
    add_macos_input_mode_property(properties);
    return make_tool(
        "browser_hover",
        interaction_description(
            "Move the real Browser pointer over a page element.",
            "Move the Browser pointer over a page element. On macOS native moves the OS pointer and requires Accessibility permission; synthetic dispatches DOM mouse events."),
        object_schema(std::move(properties)),
        false,
        [](const json& args, const ToolContext& context) {
            if (!has_target_descriptor(args)) {
                return failure_result(
                    "invalid_arguments",
                    "browser_hover requires target, selector, role, or name");
            }
            AgentBrowserCdpClient client;
            std::string error;
            if (!connect_client(client, context, error, &args)) return failure_result("desktop_browser_unavailable", error);
            json target = resolve_target(client, args, context, error);
            if (!error.empty()) return failure_result("target_not_found", error);
            const auto& rect = target["rect"];
            if (!dispatch_mouse(client, "mouseMoved", rect.value("cx", 0.0), rect.value("cy", 0.0),
                                json::object(), args, context, error)) {
                return input_failure_result("hover_failed", error);
            }
            return success_result_with_page(
                client, context,
                with_input_metadata({{"target", target}}, args), "Hovered",
                target.value("name", "element"));
        });
}

ToolImpl drag_tool() {
    json properties{
        {"from", string_property("Source @eN reference.")},
        {"to", string_property("Destination @eN reference.")},
        {"revision", integer_property("Snapshot revision for both refs.", 1, 1000000000)},
    };
    add_macos_input_mode_property(properties);
    return make_tool(
        "browser_drag",
        interaction_description(
            "Drag from one visible page element to another using real CDP pointer events.",
            "Drag from one visible page element to another. On macOS try synthetic first; choose native for trusted HTML5 or application-specific pointer gestures."),
        object_schema(std::move(properties), {"from", "to", "revision"}),
        false,
        [](const json& args, const ToolContext& context) {
            AgentBrowserCdpClient client;
            std::string error;
            if (!connect_client(client, context, error, &args)) return failure_result("desktop_browser_unavailable", error);
            json from_args{{"target", args.value("from", "")}, {"revision", args.value("revision", 0)}};
            json to_args{{"target", args.value("to", "")}, {"revision", args.value("revision", 0)}};
            json from = resolve_target(client, from_args, context, error);
            if (!error.empty()) return failure_result("target_not_found", error, "drag source");
            json to = resolve_target(client, to_args, context, error);
            if (!error.empty()) return failure_result("target_not_found", error, "drag destination");
            const double x1 = from["rect"].value("cx", 0.0);
            const double y1 = from["rect"].value("cy", 0.0);
            const double x2 = to["rect"].value("cx", 0.0);
            const double y2 = to["rect"].value("cy", 0.0);
            if (!dispatch_mouse(
                    client, "mouseMoved", x1, y1, json::object(),
                    args, context, error) ||
                !dispatch_mouse(
                    client, "mousePressed", x1, y1,
                    {{"button", "left"}, {"clickCount", 1}},
                    args, context, error)) {
                return input_failure_result("drag_failed", error);
            }
            for (int step = 1; step <= 8 && error.empty(); ++step) {
                const double ratio = static_cast<double>(step) / 8.0;
                dispatch_mouse(client, "mouseMoved",
                               x1 + (x2 - x1) * ratio,
                               y1 + (y2 - y1) * ratio,
                               {{"button", "left"}, {"buttons", 1}},
                               args, context, error);
            }
            if (!error.empty()) {
                const std::string drag_error = error;
                std::string release_error;
                dispatch_mouse(
                    client, "mouseReleased", x2, y2,
                    {{"button", "left"}, {"clickCount", 1}},
                    args, context, release_error);
                return input_failure_result("drag_failed", drag_error);
            }
            dispatch_mouse(client, "mouseReleased", x2, y2,
                           {{"button", "left"}, {"clickCount", 1}},
                           args, context, error);
            if (!error.empty()) return input_failure_result("drag_failed", error);
            return success_result_with_page(
                client, context,
                with_input_metadata(
                    {{"from", from}, {"to", to}, {"dragged", true}}, args),
                "Dragged", "Browser element");
        });
}

ToolImpl scroll_tool() {
    json properties = target_properties();
    merge_properties(properties, {
        {"delta_x", {{"type", "number"}, {"description", "Horizontal CSS pixels."}}},
        {"delta_y", {{"type", "number"}, {"description", "Vertical CSS pixels; positive scrolls down."}}},
        {"x", {{"type", "number"}, {"description", "Optional viewport X coordinate."}}},
        {"y", {{"type", "number"}, {"description", "Optional viewport Y coordinate."}}},
    });
    add_macos_input_mode_property(properties);
    return make_tool(
        "browser_scroll",
        interaction_description(
            "Scroll the visible Browser page or a target element area using a CDP mouse wheel event.",
            "Scroll the visible Browser page or a target element area. On macOS synthetic scrolls the DOM directly; choose native for a real trackpad-style wheel event."),
        object_schema(std::move(properties)),
        false,
        [](const json& args, const ToolContext& context) {
            AgentBrowserCdpClient client;
            std::string error;
            if (!connect_client(client, context, error, &args)) return failure_result("desktop_browser_unavailable", error);
            json center;
            if (has_target_descriptor(args)) {
                const json target = resolve_target(client, args, context, error);
                if (!error.empty()) {
                    return failure_result("target_not_found", error);
                }
                center = {
                    {"x", target["rect"].value("cx", 0.0)},
                    {"y", target["rect"].value("cy", 0.0)},
                };
            } else {
                center = evaluate(
                    client, "({x:innerWidth/2,y:innerHeight/2})",
                    context, error);
                if (!error.empty()) return failure_result("scroll_failed", error);
            }
            const double x = args.value("x", center.value("x", 0.0));
            const double y = args.value("y", center.value("y", 0.0));
            const double dx = args.value("delta_x", 0.0);
            const double dy = args.value("delta_y", 600.0);
            if (!dispatch_mouse(client, "mouseWheel", x, y,
                                {{"deltaX", dx}, {"deltaY", dy}},
                                args, context, error)) {
                return input_failure_result("scroll_failed", error);
            }
            return success_result_with_page(
                client, context,
                with_input_metadata(
                    {{"delta_x", dx}, {"delta_y", dy}}, args),
                "Scrolled", "Browser page");
        });
}

ToolImpl wait_tool() {
    json properties = target_properties();
    merge_properties(properties, {
        {"condition", {{"type", "string"}, {"enum", {"element_visible", "element_hidden", "text_present", "url_contains", "title_contains", "load_complete"}}}},
        {"text", string_property("Expected text for text/title/url conditions.")},
        {"timeout_ms", integer_property("Maximum wait in milliseconds.", 100, 120000)},
    });
    return make_tool(
        "browser_wait",
        "Wait until a visible-page condition becomes true. This polls the same Browser page and honors task aborts.",
        object_schema(std::move(properties), {"condition"}),
        true,
        [](const json& args, const ToolContext& context) {
            const std::string requested_condition = args.value(
                "condition", "load_complete");
            if ((requested_condition == "element_visible" ||
                 requested_condition == "element_hidden") &&
                !has_target_descriptor(args)) {
                return failure_result(
                    "invalid_arguments",
                    "element waits require target, selector, role, or name");
            }
            const int timeout_ms = args.value("timeout_ms", 10000);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
            json last_observed;
            std::string last_error;
            AgentBrowserCdpClient client;
            std::string connect_error;
            if (!connect_client(client, context, connect_error, &args)) {
                return failure_result("desktop_browser_unavailable", connect_error);
            }
            while (std::chrono::steady_clock::now() < deadline) {
                if (context.abort_flag && context.abort_flag->load()) {
                    return failure_result("aborted", "Agent Browser wait aborted");
                }
                std::string error;
                const std::string condition = requested_condition;
                const std::string expected = args.value("text", "");
                bool matched = false;
                if (condition == "element_visible" || condition == "element_hidden") {
                    json target = evaluate(client, target_resolution_script(args), context, error);
                    last_observed = target;
                    if (error.empty() && target.is_object()) {
                        if (target.value("ok", false)) {
                            const bool visible = target.value("visible", false);
                            matched = condition == "element_visible" ? visible : !visible;
                        } else if (condition == "element_hidden" &&
                                   target.value("code", "") == "target_not_found") {
                            matched = true;
                        } else {
                            last_error = target.value(
                                "message", "could not resolve browser target");
                        }
                    }
                } else {
                    json observed = evaluate(
                        client,
                        "(() => ({text:(document.body?.innerText||'').slice(0,50000),url:location.href,title:document.title,ready:document.readyState}))()",
                        context,
                        error);
                    last_observed = observed;
                    if (error.empty()) {
                        if (condition == "text_present") matched = observed.value("text", "").find(expected) != std::string::npos;
                        else if (condition == "url_contains") matched = observed.value("url", "").find(expected) != std::string::npos;
                        else if (condition == "title_contains") matched = observed.value("title", "").find(expected) != std::string::npos;
                        else if (condition == "load_complete") matched = observed.value("ready", "") == "complete";
                    }
                }
                if (matched) {
                    return success_result_with_page(
                        client, context,
                        {{"condition", condition}, {"matched", true},
                         {"observed", last_observed}},
                        "Waited", condition);
                }
                if (!error.empty()) last_error = error;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            return failure_result(
                "wait_timeout",
                "Browser condition did not match before timeout" +
                    (last_error.empty() ? std::string() : ": " + last_error));
        });
}

std::string safe_file_name(std::string value) {
    if (value.empty()) value = "browser-screenshot.png";
    value = std::filesystem::path(value).filename().string();
    for (char& ch : value) {
        if (!std::isalnum(static_cast<unsigned char>(ch)) &&
            ch != '-' && ch != '_' && ch != '.') ch = '-';
    }
    if (value.size() < 4 || value.substr(value.size() - 4) != ".png") value += ".png";
    return value;
}

ToolImpl screenshot_tool() {
    return make_tool(
        "browser_screenshot",
        "Capture the same visible Browser page as a PNG attachment. Use browser_read_page for semantic work and screenshots when visual evidence matters.",
        object_schema({
            {"file_name", string_property("Optional safe attachment file name.")},
            {"full_page", {{"type", "boolean"}, {"description", "Capture beyond the current viewport when supported."}}},
        }),
        true,
        [](const json& args, const ToolContext& context) {
            AgentBrowserCdpClient client;
            std::string error;
            if (!connect_client(client, context, error, &args)) return failure_result("desktop_browser_unavailable", error);
            json params{{"format", "png"}, {"fromSurface", true}, {"captureBeyondViewport", args.value("full_page", false)}};
            json captured = client.command("Page.captureScreenshot", params, std::chrono::seconds(30),
                                           context.abort_flag, error);
            if (!error.empty()) return failure_result("screenshot_failed", error);
            const auto bytes = decode_agent_browser_base64(captured.value("data", ""));
            const auto dimensions = bytes
                ? agent_browser_png_dimensions(*bytes)
                : std::nullopt;
            if (!bytes || !dimensions) {
                return failure_result(
                    "screenshot_failed",
                    "WebView2 returned invalid PNG screenshot data");
            }
            const std::filesystem::path root = context.scratch_dir.empty()
                ? std::filesystem::temp_directory_path() / "acecode-agent-browser"
                : acecode::path_from_utf8(context.scratch_dir);
            std::error_code filesystem_error;
            std::filesystem::create_directories(root, filesystem_error);
            const std::filesystem::path output = root / safe_file_name(args.value("file_name", ""));
            std::ofstream stream(output, std::ios::binary | std::ios::trunc);
            stream.write(reinterpret_cast<const char*>(bytes->data()),
                         static_cast<std::streamsize>(bytes->size()));
            stream.close();
            if (!stream) return failure_result("screenshot_failed", "failed to write Browser screenshot");
            json data{
                {"path", acecode::path_to_utf8(output)},
                {"mime_type", "image/png"},
                {"size_bytes", bytes->size()},
            };
            data["width"] = dimensions->first;
            data["height"] = dimensions->second;
            ToolResult result = success_result_with_page(
                client, context, std::move(data), "Captured",
                acecode::path_to_utf8(output.filename()),
                {{"bytes", std::to_string(bytes->size())}});
            result.attachments = json::array({{
                {"name", acecode::path_to_utf8(output.filename())},
                {"mime_type", "image/png"},
                {"path", acecode::path_to_utf8(output)},
            }});
            return result;
        });
}

ToolImpl dialog_tool() {
    return make_tool(
        "browser_handle_dialog",
        "Accept or dismiss the currently open JavaScript alert/confirm/prompt in the visible Browser page.",
        object_schema({
            {"accept", {{"type", "boolean"}, {"description", "Accept when true; dismiss when false."}}},
            {"prompt_text", string_property("Optional text for a JavaScript prompt.")},
        }, {"accept"}),
        false,
        [](const json& args, const ToolContext& context) {
            AgentBrowserCdpClient client;
            std::string error;
            if (!connect_client(client, context, error, &args)) return failure_result("desktop_browser_unavailable", error);
            json params{{"accept", args.value("accept", true)}};
            if (args.contains("prompt_text")) params["promptText"] = args["prompt_text"];
            client.command("Page.handleJavaScriptDialog", params, kCommandTimeout,
                           context.abort_flag, error);
            if (!error.empty()) return failure_result("dialog_failed", error);
            return success_result_with_page(
                client, context, {{"accepted", args.value("accept", true)}},
                "Handled", "Browser dialog");
        });
}

ToolImpl evaluate_tool() {
    return make_tool(
        "browser_evaluate",
        "Evaluate JavaScript in the visible Browser page and return the JSON-serializable value. This can mutate the page; prefer browser_read_page and semantic tools for ordinary work.",
        object_schema({{"code", string_property("JavaScript expression or IIFE.")}}, {"code"}),
        false,
        [](const json& args, const ToolContext& context) {
            AgentBrowserCdpClient client;
            std::string error;
            if (!connect_client(client, context, error, &args)) return failure_result("desktop_browser_unavailable", error);
            json value = evaluate(client, args.value("code", ""), context, error);
            if (!error.empty()) return failure_result("evaluation_failed", error);
            return success_result_with_page(
                client, context, {{"result", value}}, "Evaluated", "Browser page");
        });
}

ToolImpl close_tool() {
    return make_tool(
        "browser_close",
        "Close exactly one ACECode Desktop Browser page. Pass page_id to close a specific page, or omit it to close the active page.",
        object_schema(json::object()),
        false,
        [](const json& args, const ToolContext& context) {
            AgentBrowserCdpClient client;
            std::string error;
            if (!connect_client(client, context, error, &args, false)) return failure_result("desktop_browser_unavailable", error);
            if (!client.close_page(kCommandTimeout, context.abort_flag, error)) {
                return failure_result("browser_close_failed", error);
            }
            if (!error.empty()) return failure_result("browser_close_failed", error);
            return success_result({{"closed", true}}, "Closed", "Browser page",
                                  {}, client.page_id());
        });
}

} // namespace

nlohmann::json agent_browser_input_result_metadata(
    nlohmann::json value,
    const std::string& input_mode) {
#ifdef __APPLE__
    if (!value.is_object()) value = {{"result", std::move(value)}};
    const std::string mode = input_mode == "native" ? "native" : "synthetic";
    value["input_mode"] = mode;
    value["input_trust"] = mode;
#else
    (void)input_mode;
#endif
    return value;
}

std::string agent_browser_snapshot_script() {
    return R"JS(((options) => {
  const maxElements = Math.max(1, Math.min(500, Number(options.maxElements) || 300));
  const maxText = Math.max(100, Math.min(50000, Number(options.maxText) || 30000));
  const visible = (el) => {
    if (!el || !el.isConnected) return false;
    const rect = el.getBoundingClientRect();
    const style = getComputedStyle(el);
    return rect.width > 0 && rect.height > 0 && style.display !== 'none'
      && style.visibility !== 'hidden' && style.opacity !== '0';
  };
  const textOf = (el) => String(
    el.getAttribute('aria-label') || el.getAttribute('title')
    || ('value' in el ? el.value : '') || el.innerText || el.textContent || ''
  ).replace(/\s+/g, ' ').trim();
  const roleOf = (el) => el.getAttribute('role') || ({
    A: 'link', BUTTON: 'button', TEXTAREA: 'textbox', SELECT: 'combobox',
    SUMMARY: 'button', OPTION: 'option', IMG: 'img',
  }[el.tagName] || (el.tagName === 'INPUT'
    ? (el.type === 'checkbox' ? 'checkbox' : el.type === 'radio' ? 'radio' : 'textbox')
    : el.tagName.toLowerCase()));
  const candidates = Array.from(document.querySelectorAll(
    'a,button,input,textarea,select,summary,option,[role],[onclick],[contenteditable="true"],[tabindex]:not([tabindex="-1"])'
  )).filter(visible).slice(0, maxElements);
  const previous = window.__aceAgentBrowserSnapshot;
  const requestedRevision = Number(options.revision);
  const revision = Number.isInteger(requestedRevision)
    && requestedRevision > 0 && requestedRevision <= 1000000000
    ? requestedRevision
    : Number(previous?.revision || 0) + 1;
  window.__aceAgentBrowserSnapshot = { revision, nodes: candidates };
  const elements = candidates.map((el, index) => {
    const rect = el.getBoundingClientRect();
    return {
      ref: `@e${index + 1}`,
      revision,
      role: roleOf(el),
      name: textOf(el).slice(0, 220),
      tag: el.tagName.toLowerCase(),
      value: 'value' in el ? String(el.value).slice(0, 500) : undefined,
      disabled: !!el.disabled || el.getAttribute('aria-disabled') === 'true',
      checked: 'checked' in el ? !!el.checked : undefined,
      selected: 'selected' in el ? !!el.selected : undefined,
      rect: {
        x: Math.round(rect.x), y: Math.round(rect.y),
        width: Math.round(rect.width), height: Math.round(rect.height),
      },
    };
  });
  return {
    revision,
    url: location.href,
    title: document.title,
    ready_state: document.readyState,
    text: String(document.body?.innerText || '').slice(0, maxText),
    elements,
    viewport: { width: innerWidth, height: innerHeight, device_scale_factor: devicePixelRatio },
  };
}))JS";
}

std::vector<std::string> agent_browser_tool_names() {
    return {
        "browser_open",
        "browser_navigate",
        "browser_read_page",
        "browser_click",
        "browser_fill",
        "browser_type",
        "browser_press",
        "browser_hover",
        "browser_drag",
        "browser_scroll",
        "browser_wait",
        "browser_screenshot",
        "browser_handle_dialog",
        "browser_evaluate",
        "browser_close",
    };
}

void register_agent_browser_tools(ToolExecutor& tools) {
#if defined(_WIN32) || defined(__APPLE__)
    tools.register_tool(open_tool());
    tools.register_tool(navigate_tool());
    tools.register_tool(read_page_tool());
    tools.register_tool(click_tool());
    tools.register_tool(fill_tool());
    tools.register_tool(type_tool());
    tools.register_tool(press_tool());
    tools.register_tool(hover_tool());
    tools.register_tool(drag_tool());
    tools.register_tool(scroll_tool());
    tools.register_tool(wait_tool());
    tools.register_tool(screenshot_tool());
    tools.register_tool(dialog_tool());
    tools.register_tool(evaluate_tool());
    tools.register_tool(close_tool());
#else
    (void)tools;
#endif
}

std::size_t unregister_agent_browser_tools(ToolExecutor& tools) {
    std::size_t removed = 0;
    for (const auto& name : agent_browser_tool_names()) {
        if (tools.unregister_tool(name)) ++removed;
    }
    return removed;
}

} // namespace acecode::agent_browser
