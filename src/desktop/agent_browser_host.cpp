#include "agent_browser_host.hpp"

#include "agent_browser_runtime.hpp"

#include "daemon/platform.hpp"
#include "utils/encoding.hpp"
#include "utils/logger.hpp"
#include "utils/token.hpp"
#include "utils/utf8_path.hpp"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <wrl.h>
#  include <WebView2.h>
#  include <WebView2EnvironmentOptions.h>
#  include <webview/webview.h>
#endif

namespace acecode::desktop {
namespace {

std::int64_t now_unix_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void assign_error(std::string* target, const std::string& value) {
    if (target) *target = value;
}

std::string agent_browser_title_or_default(std::string title) {
    return title.find_first_not_of(" \t\r\n") == std::string::npos
        ? std::string(kAgentBrowserDefaultTitle)
        : std::move(title);
}

#ifdef _WIN32
constexpr std::size_t kAgentBrowserMaxConsoleEntries = 1000;
constexpr std::size_t kAgentBrowserMaxConsoleEntryBytes = 16 * 1024;
constexpr std::size_t kAgentBrowserMaxFaviconBytes = 256 * 1024;
constexpr char kAgentBrowserElementPickerKey[] =
    "__acecodeAgentBrowserElementPickerV1";

bool valid_agent_browser_favicon(const std::string& value) {
    if (value.empty() || value.size() > kAgentBrowserMaxFaviconBytes) {
        return false;
    }
    return value.rfind("https://", 0) == 0 ||
        value.rfind("http://", 0) == 0 ||
        value.rfind("data:image/", 0) == 0;
}

const char* agent_browser_favicon_expression() {
    return R"JS((async () => {
  const maxDataUrlLength = 256 * 1024;
  const links = [...document.querySelectorAll('link[rel][href]')];
  const icon = links.find((link) => String(link.rel || '').toLowerCase().split(/\s+/).includes('icon'));
  let href = icon?.href || '';
  if (!href && (location.protocol === 'http:' || location.protocol === 'https:')) {
    href = new URL('/favicon.ico', location.href).href;
  }
  if (!href) return '';
  try {
    const parsed = new URL(href, location.href);
    if (!['http:', 'https:', 'data:'].includes(parsed.protocol)) return '';
    href = parsed.href;
  } catch (_) {
    return '';
  }
  if (href.startsWith('data:')) {
    return href.startsWith('data:image/') && href.length <= maxDataUrlLength ? href : '';
  }
  if (new URL(href).origin !== location.origin) {
    return href.length <= 4096 ? href : '';
  }
  try {
    const response = await fetch(href, { credentials: 'include', cache: 'force-cache' });
    if (!response.ok) return href.length <= 4096 ? href : '';
    const declaredLength = Number(response.headers.get('content-length') || 0);
    if (declaredLength > 128 * 1024) return href.length <= 4096 ? href : '';
    const blob = await response.blob();
    if (!String(blob.type || '').toLowerCase().startsWith('image/') || blob.size > 128 * 1024) {
      return href.length <= 4096 ? href : '';
    }
    const dataUrl = await new Promise((resolve) => {
      const reader = new FileReader();
      reader.onload = () => resolve(typeof reader.result === 'string' ? reader.result : '');
      reader.onerror = () => resolve('');
      reader.readAsDataURL(blob);
    });
    return dataUrl.length <= maxDataUrlLength ? dataUrl : (href.length <= 4096 ? href : '');
  } catch (_) {
    return href.length <= 4096 ? href : '';
  }
})())JS";
}

const char* agent_browser_web_error_kind(
    COREWEBVIEW2_WEB_ERROR_STATUS status) {
    switch (status) {
    case COREWEBVIEW2_WEB_ERROR_STATUS_CERTIFICATE_COMMON_NAME_IS_INCORRECT:
    case COREWEBVIEW2_WEB_ERROR_STATUS_CERTIFICATE_EXPIRED:
    case COREWEBVIEW2_WEB_ERROR_STATUS_CLIENT_CERTIFICATE_CONTAINS_ERRORS:
    case COREWEBVIEW2_WEB_ERROR_STATUS_CERTIFICATE_REVOKED:
    case COREWEBVIEW2_WEB_ERROR_STATUS_CERTIFICATE_IS_INVALID:
        return "certificate";
    case COREWEBVIEW2_WEB_ERROR_STATUS_SERVER_UNREACHABLE:
        return "server_unreachable";
    case COREWEBVIEW2_WEB_ERROR_STATUS_TIMEOUT:
        return "timeout";
    case COREWEBVIEW2_WEB_ERROR_STATUS_ERROR_HTTP_INVALID_SERVER_RESPONSE:
        return "invalid_response";
    case COREWEBVIEW2_WEB_ERROR_STATUS_CONNECTION_ABORTED:
        return "connection_aborted";
    case COREWEBVIEW2_WEB_ERROR_STATUS_CONNECTION_RESET:
        return "connection_reset";
    case COREWEBVIEW2_WEB_ERROR_STATUS_DISCONNECTED:
        return "disconnected";
    case COREWEBVIEW2_WEB_ERROR_STATUS_CANNOT_CONNECT:
        return "cannot_connect";
    case COREWEBVIEW2_WEB_ERROR_STATUS_HOST_NAME_NOT_RESOLVED:
        return "name_not_resolved";
    case COREWEBVIEW2_WEB_ERROR_STATUS_OPERATION_CANCELED:
        return "cancelled";
    case COREWEBVIEW2_WEB_ERROR_STATUS_REDIRECT_FAILED:
        return "redirect_failed";
    case COREWEBVIEW2_WEB_ERROR_STATUS_VALID_AUTHENTICATION_CREDENTIALS_REQUIRED:
        return "authentication_required";
    case COREWEBVIEW2_WEB_ERROR_STATUS_VALID_PROXY_AUTHENTICATION_REQUIRED:
        return "proxy_authentication_required";
    case COREWEBVIEW2_WEB_ERROR_STATUS_UNEXPECTED_ERROR:
    case COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN:
    default:
        return "unexpected";
    }
}

const char* agent_browser_process_failure_kind(
    COREWEBVIEW2_PROCESS_FAILED_KIND kind,
    COREWEBVIEW2_PROCESS_FAILED_REASON reason) {
    const bool top_level_failure =
        kind == COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED ||
        kind == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED ||
        kind == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_UNRESPONSIVE;
    if (!top_level_failure) return "";
    if (reason == COREWEBVIEW2_PROCESS_FAILED_REASON_OUT_OF_MEMORY) {
        return "out_of_memory";
    }
    if (kind == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_UNRESPONSIVE ||
        reason == COREWEBVIEW2_PROCESS_FAILED_REASON_UNRESPONSIVE) {
        return "unresponsive";
    }
    if (kind == COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED) {
        return "browser_process_exited";
    }
    if (kind == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED) {
        return "render_process_exited";
    }
    return "";
}

std::string clip_agent_browser_text(std::string value,
                                    std::size_t max_bytes) {
    if (value.size() <= max_bytes) return value;
    value.resize(max_bytes);
    value += "\n[truncated]";
    return value;
}

std::string cdp_remote_object_text(const nlohmann::json& object) {
    if (!object.is_object()) return object.dump();
    if (object.contains("value") && !object["value"].is_null()) {
        return object["value"].is_string()
            ? object["value"].get<std::string>()
            : object["value"].dump();
    }
    if (object.contains("unserializableValue") &&
        object["unserializableValue"].is_string()) {
        return object["unserializableValue"].get<std::string>();
    }
    if (object.contains("description") && object["description"].is_string()) {
        return object["description"].get<std::string>();
    }
    return object.value("type", std::string("value"));
}

const char* agent_browser_element_picker_expression() {
    return R"JS((() => {
  const key = '__acecodeAgentBrowserElementPickerV1';
  const previous = globalThis[key];
  if (previous && typeof previous.cancel === 'function') previous.cancel();
  return new Promise((resolve) => {
    const root = document.documentElement || document.body;
    if (!root) { resolve({ cancelled: true }); return; }
    let finished = false;
    let commitPending = false;
    let dragStart = null;
    let dragTarget = null;
    let highlighted = null;
    const host = document.createElement('div');
    host.setAttribute('data-acecode-element-picker', '');
    host.style.cssText = 'position:fixed;inset:0;width:0;height:0;z-index:2147483647;pointer-events:none';
    const shadow = host.attachShadow({ mode: 'closed' });
    const styleNode = document.createElement('style');
    styleNode.textContent = `
      .box{display:none;position:fixed;box-sizing:border-box;border:2px solid #0e70c0;background:rgba(14,112,192,.14);pointer-events:none;z-index:2}
      .label{display:none;position:fixed;max-width:70vw;padding:3px 6px;border-radius:3px;background:#0e70c0;color:#fff;font:11px/16px "Segoe UI",sans-serif;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;pointer-events:none;z-index:3;box-shadow:0 2px 8px rgba(0,0,0,.22)}
      .drag{display:none;position:fixed;box-sizing:border-box;border:1px dashed #0e70c0;background:rgba(14,112,192,.08);pointer-events:none;z-index:1}
    `;
    const box = document.createElement('div'); box.className = 'box';
    const label = document.createElement('div'); label.className = 'label';
    const drag = document.createElement('div'); drag.className = 'drag';
    shadow.append(styleNode, box, drag, label);
    root.appendChild(host);

    const cursor = document.createElement('style');
    cursor.setAttribute('data-acecode-element-picker-cursor', '');
    cursor.textContent = '*{cursor:default!important}';
    (document.head || root).appendChild(cursor);

    const clip = (value, limit) => {
      const text = String(value == null ? '' : value);
      return text.length > limit ? `${text.slice(0, limit)}\n[truncated]` : text;
    };
    const elementAt = (x, y) => {
      const values = document.elementsFromPoint(x, y);
      return values.find((value) => value !== host && !host.contains(value));
    };
    const commonAncestor = (values) => {
      const unique = [...new Set(values.filter(Boolean))];
      if (!unique.length) return null;
      let chain = [];
      for (let node = unique[0]; node; node = node.parentElement) chain.unshift(node);
      for (let i = 1; i < unique.length && chain.length; i += 1) {
        const other = [];
        for (let node = unique[i]; node; node = node.parentElement) other.unshift(node);
        let index = 0;
        while (index < chain.length && index < other.length && chain[index] === other[index]) index += 1;
        chain = chain.slice(0, index);
      }
      return chain[chain.length - 1] || null;
    };
    const regionTarget = (left, top, width, height) => {
      const right = left + width;
      const bottom = top + height;
      const centerX = left + width / 2;
      const centerY = top + height / 2;
      return commonAncestor([
        elementAt(left, top), elementAt(right, top), elementAt(left, bottom),
        elementAt(right, bottom), elementAt(centerX, top), elementAt(centerX, bottom),
        elementAt(left, centerY), elementAt(right, centerY), elementAt(centerX, centerY),
      ]);
    };
    const selectorName = (element) => {
      if (!element) return '';
      const tag = String(element.tagName || 'element').toLowerCase();
      const id = element.id ? `#${element.id}` : '';
      const classes = [...element.classList].slice(0, 4).map((name) => `.${name}`).join('');
      return `${tag}${id}${classes}`;
    };
    const render = (element) => {
      highlighted = element || null;
      if (!element) { box.style.display = 'none'; label.style.display = 'none'; return; }
      const rect = element.getBoundingClientRect();
      box.style.display = 'block';
      box.style.left = `${rect.left}px`; box.style.top = `${rect.top}px`;
      box.style.width = `${Math.max(0, rect.width)}px`; box.style.height = `${Math.max(0, rect.height)}px`;
      label.textContent = `${selectorName(element)}  ${Math.round(rect.width)} × ${Math.round(rect.height)}`;
      label.style.display = 'block';
      label.style.left = `${Math.max(2, Math.min(rect.left, innerWidth - 180))}px`;
      label.style.top = `${Math.max(2, rect.top >= 24 ? rect.top - 22 : rect.bottom + 2)}px`;
    };
    const collect = (element) => {
      const rect = element.getBoundingClientRect();
      const attributes = {};
      [...element.attributes].slice(0, 100).forEach((attr) => { attributes[attr.name] = clip(attr.value, 2048); });
      const computed = getComputedStyle(element);
      const computedStyles = {};
      const cssLines = [];
      for (let i = 0; i < computed.length && i < 200; i += 1) {
        const name = computed[i];
        const value = clip(computed.getPropertyValue(name), 512);
        computedStyles[name] = value;
        cssLines.push(`${name}: ${value};`);
      }
      const ancestors = [];
      for (let node = element; node && ancestors.length < 16; node = node.parentElement) {
        ancestors.unshift({
          tagName: String(node.tagName || '').toLowerCase(),
          id: clip(node.id || '', 512),
          classNames: [...node.classList].slice(0, 12).map((name) => clip(name, 256)),
        });
      }
      return {
        url: location.href,
        title: document.title,
        name: selectorName(element),
        tagName: String(element.tagName || '').toLowerCase(),
        outerHTML: clip(element.outerHTML || '', 40000),
        innerText: clip(element.innerText || element.textContent || '', 20000),
        attributes,
        computedStyle: clip(cssLines.join('\n'), 40000),
        computedStyles,
        ancestors,
        bounds: { x: rect.left, y: rect.top, width: rect.width, height: rect.height },
        dimensions: { top: rect.top, left: rect.left, width: rect.width, height: rect.height },
      };
    };
    const listeners = [
      ['pointermove', onPointerMove], ['pointerdown', onPointerDown],
      ['pointerup', onPointerUp], ['click', suppress], ['contextmenu', suppress],
      ['keydown', onKeyDown], ['scroll', onViewportChange], ['resize', onViewportChange],
    ];
    function cleanup() {
      listeners.forEach(([name, handler]) => window.removeEventListener(name, handler, true));
      cursor.remove(); host.remove();
      try { delete globalThis[key]; } catch (_) { globalThis[key] = undefined; }
    }
    function finish(value) {
      if (finished) return;
      finished = true; cleanup(); resolve(value);
    }
    function suppress(event) {
      event.preventDefault(); event.stopImmediatePropagation();
    }
    function onPointerMove(event) {
      suppress(event);
      if (commitPending) return;
      if (!dragStart) { render(elementAt(event.clientX, event.clientY)); return; }
      const left = Math.min(dragStart.x, event.clientX);
      const top = Math.min(dragStart.y, event.clientY);
      const width = Math.abs(event.clientX - dragStart.x);
      const height = Math.abs(event.clientY - dragStart.y);
      if (width < 4 && height < 4) return;
      drag.style.display = 'block'; drag.style.left = `${left}px`; drag.style.top = `${top}px`;
      drag.style.width = `${width}px`; drag.style.height = `${height}px`;
      render(regionTarget(left, top, width, height));
    }
    function onPointerDown(event) {
      if (commitPending) { suppress(event); return; }
      if (event.button !== 0) { suppress(event); return; }
      dragStart = { x: event.clientX, y: event.clientY };
      dragTarget = elementAt(event.clientX, event.clientY);
      cursor.textContent = '*{cursor:crosshair!important}';
      suppress(event);
    }
    function onPointerUp(event) {
      if (!dragStart) { suppress(event); return; }
      const start = dragStart; dragStart = null;
      const width = Math.abs(event.clientX - start.x);
      const height = Math.abs(event.clientY - start.y);
      const target = width < 4 && height < 4
        ? (dragTarget || elementAt(event.clientX, event.clientY))
        : regionTarget(Math.min(start.x, event.clientX), Math.min(start.y, event.clientY), width, height);
      dragTarget = null; suppress(event);
      if (target) {
        const selection = { cancelled: false, element: collect(target) };
        commitPending = true;
        requestAnimationFrame(() => finish(selection));
      }
    }
    function onKeyDown(event) {
      if (event.key === 'Escape') { suppress(event); finish({ cancelled: true }); }
    }
    function onViewportChange() { if (highlighted) render(highlighted); }
    listeners.forEach(([name, handler]) => window.addEventListener(name, handler, true));
    const controller = { cancel: () => finish({ cancelled: true }) };
    try { Object.defineProperty(globalThis, key, { configurable: true, value: controller }); }
    catch (_) { globalThis[key] = controller; }
  });
})())JS";
}

constexpr wchar_t kAgentBrowserWidgetClassName[] =
    L"ACECodeAgentBrowserWidget";

HWND create_agent_browser_widget(HWND parent) {
    const HINSTANCE instance = ::GetModuleHandleW(nullptr);
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.hInstance = instance;
    window_class.lpfnWndProc = ::DefWindowProcW;
    window_class.lpszClassName = kAgentBrowserWidgetClassName;
    window_class.hCursor =
        ::LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    window_class.hbrBackground =
        reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    if (!::RegisterClassExW(&window_class) &&
        ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return nullptr;
    }

    // Keep the parent HWND visible while WebView2 creates controllers. A
    // hidden parent can leave a controller permanently unpainted on some
    // WebView2 runtimes. The parked 1x1 child is outside the client area and
    // therefore cannot cover the main ACECode WebView.
    return ::CreateWindowExW(
        WS_EX_CONTROLPARENT | WS_EX_NOPARENTNOTIFY,
        kAgentBrowserWidgetClassName,
        L"",
        WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | WS_VISIBLE,
        -1,
        -1,
        1,
        1,
        parent,
        nullptr,
        instance,
        nullptr);
}

void park_agent_browser_widget(HWND widget) {
    if (!widget || !::IsWindow(widget)) return;
    ::SetWindowPos(widget,
                   HWND_TOP,
                   -1,
                   -1,
                   1,
                   1,
                   SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void hide_agent_browser_widget(HWND widget) {
    if (!widget || !::IsWindow(widget)) return;
    ::SetWindowPos(widget,
                   nullptr,
                   0,
                   0,
                   0,
                   0,
                   SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE |
                       SWP_NOZORDER | SWP_HIDEWINDOW);
}

bool apply_agent_browser_widget_region(
    HWND widget,
    int width,
    int height,
    const std::vector<AgentBrowserOcclusionRect>& occlusions) {
    if (!widget || !::IsWindow(widget)) return false;
    if (occlusions.empty()) {
        return ::SetWindowRgn(widget, nullptr, TRUE) != 0;
    }

    HRGN visible_region = ::CreateRectRgn(0, 0, width, height);
    if (!visible_region) return false;
    for (const auto& occlusion : occlusions) {
        const int left = (std::max)(0, (std::min)(width, occlusion.x));
        const int top = (std::max)(0, (std::min)(height, occlusion.y));
        const int right = (std::max)(left, (std::min)(
            width, occlusion.x + occlusion.width));
        const int bottom = (std::max)(top, (std::min)(
            height, occlusion.y + occlusion.height));
        if (right <= left || bottom <= top) continue;

        HRGN hole = ::CreateRectRgn(left, top, right, bottom);
        if (!hole) {
            ::DeleteObject(visible_region);
            return false;
        }
        const int combine_result =
            ::CombineRgn(visible_region, visible_region, hole, RGN_DIFF);
        ::DeleteObject(hole);
        if (combine_result == ERROR) {
            ::DeleteObject(visible_region);
            return false;
        }
    }

    // SetWindowRgn takes ownership only on success.
    if (::SetWindowRgn(widget, visible_region, TRUE) == 0) {
        ::DeleteObject(visible_region);
        return false;
    }
    return true;
}

std::string hresult_text(HRESULT result) {
    return "HRESULT 0x" + [] (unsigned long value) {
        constexpr char digits[] = "0123456789ABCDEF";
        std::string output(8, '0');
        for (int index = 7; index >= 0; --index) {
            output[static_cast<std::size_t>(index)] = digits[value & 0xF];
            value >>= 4;
        }
        return output;
    }(static_cast<unsigned long>(result));
}

bool proxy_aborted(const std::atomic<bool>& stopping,
                   std::chrono::steady_clock::time_point deadline) {
    return stopping.load() || std::chrono::steady_clock::now() >= deadline;
}

bool pipe_transfer(HANDLE pipe,
                   void* buffer,
                   std::size_t size,
                   bool write,
                   const std::atomic<bool>& stopping,
                   std::chrono::steady_clock::time_point deadline) {
    std::size_t offset = 0;
    while (offset < size && !proxy_aborted(stopping, deadline)) {
        OVERLAPPED operation{};
        operation.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!operation.hEvent) return false;
        const DWORD chunk = static_cast<DWORD>((std::min)(
            size - offset, static_cast<std::size_t>(1024u * 1024u)));
        DWORD transferred = 0;
        BOOL started = write
            ? ::WriteFile(pipe,
                          static_cast<const char*>(buffer) + offset,
                          chunk, &transferred, &operation)
            : ::ReadFile(pipe,
                         static_cast<char*>(buffer) + offset,
                         chunk, &transferred, &operation);
        DWORD operation_error = started ? ERROR_SUCCESS : ::GetLastError();
        bool pending_io = false;
        if (!started && operation_error == ERROR_IO_PENDING) {
            pending_io = true;
            while (!proxy_aborted(stopping, deadline)) {
                const DWORD wait = ::WaitForSingleObject(operation.hEvent, 50);
                if (wait == WAIT_OBJECT_0) {
                    started = ::GetOverlappedResult(
                        pipe, &operation, &transferred, FALSE);
                    operation_error = started ? ERROR_SUCCESS : ::GetLastError();
                    break;
                }
                if (wait == WAIT_FAILED) {
                    operation_error = ::GetLastError();
                    break;
                }
            }
        }
        if (!started || proxy_aborted(stopping, deadline)) {
            if (pending_io) {
                ::CancelIoEx(pipe, &operation);
                ::WaitForSingleObject(operation.hEvent, INFINITE);
            }
            ::CloseHandle(operation.hEvent);
            if (!stopping.load() &&
                std::chrono::steady_clock::now() < deadline) {
                LOG_WARN(
                    std::string("[agent-browser] proxy pipe ") +
                    (write ? "write" : "read") + " failed (Windows error " +
                    std::to_string(operation_error) + ")");
            }
            return false;
        }
        ::CloseHandle(operation.hEvent);
        if (transferred == 0) return false;
        offset += transferred;
    }
    return offset == size;
}

bool connect_pipe(HANDLE pipe,
                  const std::atomic<bool>& stopping) {
    OVERLAPPED operation{};
    operation.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!operation.hEvent) return false;
    BOOL connected = ::ConnectNamedPipe(pipe, &operation);
    const DWORD error = connected ? ERROR_SUCCESS : ::GetLastError();
    bool pending_io = false;
    if (!connected && error == ERROR_PIPE_CONNECTED) connected = TRUE;
    if (!connected && error == ERROR_IO_PENDING) {
        pending_io = true;
        while (!stopping.load()) {
            const DWORD wait = ::WaitForSingleObject(operation.hEvent, 50);
            if (wait == WAIT_OBJECT_0) {
                DWORD transferred = 0;
                connected = ::GetOverlappedResult(
                    pipe, &operation, &transferred, FALSE);
                break;
            }
            if (wait == WAIT_FAILED) break;
        }
    }
    if (!connected) {
        if (pending_io) {
            ::CancelIoEx(pipe, &operation);
            ::WaitForSingleObject(operation.hEvent, INFINITE);
        }
    }
    ::CloseHandle(operation.hEvent);
    return connected != FALSE && !stopping.load();
}
#endif

} // namespace

struct AgentBrowserHost::Impl
    : public std::enable_shared_from_this<AgentBrowserHost::Impl> {
    struct PendingProxyCall {
        std::mutex mutex;
        std::condition_variable ready;
        bool completed = false;
        nlohmann::json response;
    };

    void* parent_window = nullptr;
    std::int64_t desktop_pid = 0;
    std::string desktop_instance_id;
    std::string acecode_dir;
    StateHandler state_handler;
    DispatchHandler dispatch_handler;
    mutable std::mutex state_mutex;
    AgentBrowserState host_state;
    bool parent_surface_visible = true;

#ifdef _WIN32
    struct QueuedCdpCall {
        std::string method;
        nlohmann::json params;
        std::shared_ptr<PendingProxyCall> pending;
    };

    struct Page {
        std::string id;
        AgentBrowserState state;
        AgentBrowserBounds requested_bounds;
        bool creation_started = false;
        bool closing = false;
        Microsoft::WRL::ComPtr<ICoreWebView2Controller> controller;
        Microsoft::WRL::ComPtr<ICoreWebView2> webview;
        Microsoft::WRL::ComPtr<ICoreWebView2DevToolsProtocolEventReceiver>
            runtime_console_receiver;
        Microsoft::WRL::ComPtr<ICoreWebView2DevToolsProtocolEventReceiver>
            runtime_exception_receiver;
        Microsoft::WRL::ComPtr<ICoreWebView2DevToolsProtocolEventReceiver>
            log_entry_receiver;
        EventRegistrationToken navigation_starting_token{};
        EventRegistrationToken navigation_completed_token{};
        EventRegistrationToken source_changed_token{};
        EventRegistrationToken history_changed_token{};
        EventRegistrationToken title_changed_token{};
        EventRegistrationToken new_window_token{};
        EventRegistrationToken process_failed_token{};
        EventRegistrationToken runtime_console_token{};
        EventRegistrationToken runtime_exception_token{};
        EventRegistrationToken log_entry_token{};
        std::vector<std::string> console_logs;
        std::uint64_t element_selection_generation = 0;
        std::uint64_t favicon_generation = 0;
        std::uint64_t current_navigation_id = 0;
        std::string content_state_before_navigation =
            kAgentBrowserContentStateEmpty;
        std::string favicon_before_navigation;
        std::vector<QueuedCdpCall> queued_cdp;
    };

    Microsoft::WRL::ComPtr<ICoreWebView2Environment> environment;
    webview::detail::mswebview2::loader loader;
    HWND browser_widget = nullptr;
    std::unordered_map<std::string, std::shared_ptr<Page>> pages;
    std::vector<std::string> page_order;
    std::string active_page;
    std::uint64_t next_page_sequence = 0;
    std::string proxy_pipe_name;
    std::string proxy_auth_token;
    std::atomic<bool> proxy_stopping{false};
    std::thread proxy_thread;
#endif

    Impl(void* parent,
         std::int64_t pid,
         std::string instance_id,
         StateHandler handler,
         DispatchHandler dispatcher,
         std::string data_dir)
        : parent_window(parent),
          desktop_pid(pid),
          desktop_instance_id(std::move(instance_id)),
          acecode_dir(std::move(data_dir)),
          state_handler(std::move(handler)),
          dispatch_handler(std::move(dispatcher)) {
#ifdef _WIN32
        host_state.supported = parent_window != nullptr;
#else
        host_state.error =
            "Agent Browser is available on Windows and macOS 14+ Desktop only";
#endif
    }

    ~Impl() {
#ifdef _WIN32
        proxy_stopping.store(true);
        if (proxy_thread.joinable()) proxy_thread.join();
        std::vector<std::shared_ptr<Page>> remaining;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            for (const auto& [id, page] : pages) remaining.push_back(page);
            pages.clear();
            page_order.clear();
            active_page.clear();
        }
        for (const auto& page : remaining) teardown_page(page);
        environment.Reset();
        if (browser_widget && ::IsWindow(browser_widget)) {
            ::DestroyWindow(browser_widget);
        }
        browser_widget = nullptr;
#endif
        cleanup_agent_browser_runtime_manifest(desktop_instance_id, acecode_dir);
    }

    void emit_state(const AgentBrowserState& state) const {
        if (state_handler) state_handler(state);
    }

    AgentBrowserState state(const std::string& requested_page = {}) const {
        std::lock_guard<std::mutex> lock(state_mutex);
#ifdef _WIN32
        const std::string id = requested_page.empty() ? active_page : requested_page;
        const auto found = pages.find(id);
        if (found != pages.end()) return found->second->state;
#else
        (void)requested_page;
#endif
        return host_state;
    }

    std::vector<AgentBrowserState> states() const {
        std::vector<AgentBrowserState> result;
        std::lock_guard<std::mutex> lock(state_mutex);
#ifdef _WIN32
        result.reserve(page_order.size());
        for (const std::string& id : page_order) {
            const auto found = pages.find(id);
            if (found != pages.end()) result.push_back(found->second->state);
        }
#endif
        return result;
    }

    std::string active_page_id() const {
        std::lock_guard<std::mutex> lock(state_mutex);
#ifdef _WIN32
        return active_page;
#else
        return {};
#endif
    }

    void update_host_state(
        const std::function<void(AgentBrowserState&)>& update) {
        AgentBrowserState snapshot;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            update(host_state);
            snapshot = host_state;
        }
        emit_state(snapshot);
    }

#ifdef _WIN32
    std::shared_ptr<Page> find_page(const std::string& requested_page) const {
        std::lock_guard<std::mutex> lock(state_mutex);
        const std::string id = requested_page.empty() ? active_page : requested_page;
        const auto found = pages.find(id);
        return found == pages.end() ? nullptr : found->second;
    }

    bool page_shared_with_agent(const std::shared_ptr<Page>& page) const {
        if (!page) return false;
        std::lock_guard<std::mutex> lock(state_mutex);
        return !page->closing && page->state.shared_with_agent;
    }

    bool require_agent_shared_page(
        const std::shared_ptr<Page>& page,
        const std::string& page_id,
        const std::shared_ptr<PendingProxyCall>& pending) const {
        if (page_shared_with_agent(page)) return true;
        finish_proxy_call(
            pending,
            {{"ok", false},
             {"page_id", page_id},
             {"error", "page_not_shared_with_agent"}});
        return false;
    }

    void update_page(
        const std::shared_ptr<Page>& page,
        const std::function<void(AgentBrowserState&)>& update) {
        if (!page) return;
        AgentBrowserState snapshot;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            if (page->closing && !page->state.closed) return;
            update(page->state);
            snapshot = page->state;
        }
        emit_state(snapshot);
    }
#endif

    void fail(const std::string& message) {
        LOG_ERROR("[agent-browser] " + message);
        update_host_state([&](AgentBrowserState& state) {
            state.ready = false;
            state.loading = false;
            state.error = message;
        });
#ifdef _WIN32
        for (const auto& page_state : states()) {
            if (auto page = find_page(page_state.page_id)) {
                update_page(page, [&](AgentBrowserState& state) {
                    state.ready = false;
                    state.loading = false;
                    state.error = message;
                });
            }
        }
#endif
    }

    static void finish_proxy_call(
        const std::shared_ptr<PendingProxyCall>& pending,
        nlohmann::json response) {
        {
            std::lock_guard<std::mutex> lock(pending->mutex);
            if (pending->completed) return;
            pending->completed = true;
            pending->response = std::move(response);
        }
        pending->ready.notify_all();
    }

#ifdef _WIN32
    void start() {
        if (!parent_window || !::IsWindow(static_cast<HWND>(parent_window))) {
            fail("Agent Browser parent window is unavailable");
            return;
        }
        browser_widget = create_agent_browser_widget(
            static_cast<HWND>(parent_window));
        if (!browser_widget) {
            fail("failed to create Agent Browser native child host (Windows error " +
                 std::to_string(::GetLastError()) + ")");
            return;
        }
        const auto user_data_path = agent_browser_user_data_path(acecode_dir);
        std::error_code ec;
        std::filesystem::create_directories(user_data_path, ec);
        if (ec) {
            fail("failed to create Agent Browser profile: " + ec.message());
            return;
        }
        auto options = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
        const auto weak = weak_from_this();
        auto completed = Microsoft::WRL::Callback<
            ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [weak](HRESULT result, ICoreWebView2Environment* value) -> HRESULT {
                if (const auto self = weak.lock()) {
                    self->environment_created(result, value);
                }
                return S_OK;
            });
        const HRESULT result = loader.create_environment_with_options(
            nullptr, user_data_path.wstring().c_str(), options.Get(),
            completed.Get());
        if (FAILED(result)) {
            fail("failed to start Agent Browser WebView2 environment (" +
                 hresult_text(result) + ")");
        }
    }

    bool publish_proxy() {
        if (!dispatch_handler) {
            fail("Agent Browser UI dispatcher is unavailable");
            return false;
        }
        proxy_pipe_name = "\\\\.\\pipe\\ACECode-AgentBrowser-" +
                          std::to_string(desktop_pid) + "-" +
                          desktop_instance_id;
        try {
            proxy_auth_token = acecode::generate_auth_token();
            proxy_thread = std::thread([this] { proxy_loop(); });
        } catch (const std::exception& error) {
            fail(std::string("failed to start Agent Browser proxy: ") +
                 error.what());
            return false;
        }

        AgentBrowserRuntimeManifest manifest;
        manifest.desktop_pid = desktop_pid;
        manifest.desktop_instance_id = desktop_instance_id;
        manifest.user_data_dir = acecode::path_to_utf8(
            agent_browser_user_data_path(acecode_dir));
        manifest.pipe_name = proxy_pipe_name;
        manifest.auth_token = proxy_auth_token;
        manifest.ready_at_ms = now_unix_ms();
        if (!write_agent_browser_runtime_manifest(manifest, acecode_dir)) {
            fail("failed to publish Agent Browser runtime endpoint");
            return false;
        }
        return true;
    }

    void environment_created(HRESULT result, ICoreWebView2Environment* value) {
        if (FAILED(result) || !value) {
            fail("Agent Browser WebView2 environment initialization failed (" +
                 hresult_text(result) + ")");
            return;
        }
        environment = value;
        if (!publish_proxy()) return;
        update_host_state([](AgentBrowserState& state) {
            state.ready = true;
            state.error.clear();
        });
        std::vector<std::shared_ptr<Page>> pending_pages;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            for (const auto& [id, page] : pages) pending_pages.push_back(page);
        }
        for (const auto& page : pending_pages) begin_create_controller(page);
        LOG_INFO("[agent-browser] WebView2 environment ready; Desktop proxy published");
    }

    std::string create_page_on_ui(bool shared_with_agent) {
        auto page = std::make_shared<Page>();
        page->id = "browser-" + std::to_string(desktop_pid) + "-" +
                   std::to_string(++next_page_sequence);
        page->state.page_id = page->id;
        page->state.supported = true;
        page->state.shared_with_agent = shared_with_agent;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            pages.emplace(page->id, page);
            page_order.push_back(page->id);
        }
        emit_state(page->state);
        std::string ignored;
        select_page_on_ui(page->id, &ignored);
        if (environment) begin_create_controller(page);
        return page->id;
    }

    void begin_create_controller(const std::shared_ptr<Page>& page) {
        if (!page || page->creation_started || page->closing || !environment) return;
        page->creation_started = true;
        const auto weak = weak_from_this();
        const std::string page_id = page->id;
        auto completed = Microsoft::WRL::Callback<
            ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [weak, page_id](HRESULT result,
                            ICoreWebView2Controller* value) -> HRESULT {
                if (const auto self = weak.lock()) {
                    self->controller_created(page_id, result, value);
                } else if (value) {
                    value->Close();
                }
                return S_OK;
            });
        park_agent_browser_widget(browser_widget);
        const HRESULT result = environment->CreateCoreWebView2Controller(
            browser_widget, completed.Get());
        if (FAILED(result)) {
            page_failed(page, "failed to create Agent Browser controller (" +
                              hresult_text(result) + ")");
        }
    }

    void page_failed(const std::shared_ptr<Page>& page,
                     const std::string& message) {
        LOG_ERROR("[agent-browser] " + page->id + ": " + message);
        update_page(page, [&](AgentBrowserState& state) {
            state.ready = false;
            state.loading = false;
            state.error = message;
        });
        std::vector<QueuedCdpCall> queued;
        queued.swap(page->queued_cdp);
        for (auto& call : queued) {
            finish_proxy_call(call.pending,
                              {{"ok", false},
                               {"page_id", page->id},
                               {"error", message}});
        }
    }

    void controller_created(const std::string& page_id,
                            HRESULT result,
                            ICoreWebView2Controller* value) {
        auto page = find_page(page_id);
        if (!page || page->closing) {
            if (value) value->Close();
            return;
        }
        if (FAILED(result) || !value) {
            page_failed(page,
                        "Agent Browser controller initialization failed (" +
                        hresult_text(result) + ")");
            return;
        }
        page->controller = value;
        if (FAILED(page->controller->get_CoreWebView2(&page->webview)) ||
            !page->webview) {
            page_failed(page, "Agent Browser page initialization failed");
            return;
        }

        Microsoft::WRL::ComPtr<ICoreWebView2Settings> settings;
        if (SUCCEEDED(page->webview->get_Settings(&settings)) && settings) {
            settings->put_AreDevToolsEnabled(TRUE);
            // The main ACECode WebView suppresses its default context menu,
            // but Browser pages are ordinary websites and must retain the
            // browser-native menu supplied by the installed WebView2 runtime.
            settings->put_AreDefaultContextMenusEnabled(TRUE);
            settings->put_IsStatusBarEnabled(FALSE);
            settings->put_IsScriptEnabled(TRUE);
            settings->put_AreDefaultScriptDialogsEnabled(TRUE);
            Microsoft::WRL::ComPtr<ICoreWebView2Settings3> settings3;
            if (SUCCEEDED(settings.As(&settings3)) && settings3) {
                settings3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
            }
        }
        install_events(page);
        install_console_capture(page);
        apply_bounds(page);
        update_page(page, [](AgentBrowserState& state) {
            state.ready = true;
            state.error.clear();
        });

        std::vector<QueuedCdpCall> queued;
        queued.swap(page->queued_cdp);
        for (auto& call : queued) {
            call_cdp_on_ui(page->id, call.method, call.params, call.pending);
        }
        LOG_INFO("[agent-browser] page ready: " + page->id);
    }

    void append_console_log(const std::shared_ptr<Page>& page,
                            std::string entry) {
        if (!page || entry.empty()) return;
        entry = clip_agent_browser_text(
            std::move(entry), kAgentBrowserMaxConsoleEntryBytes);
        std::lock_guard<std::mutex> lock(state_mutex);
        if (page->closing) return;
        page->console_logs.push_back(std::move(entry));
        if (page->console_logs.size() > kAgentBrowserMaxConsoleEntries) {
            page->console_logs.erase(
                page->console_logs.begin(),
                page->console_logs.begin() + static_cast<std::ptrdiff_t>(
                    page->console_logs.size() -
                    kAgentBrowserMaxConsoleEntries));
        }
    }

    void clear_console_logs(const std::shared_ptr<Page>& page) {
        if (!page) return;
        std::lock_guard<std::mutex> lock(state_mutex);
        page->console_logs.clear();
    }

    std::string console_logs_for_page(
        const std::shared_ptr<Page>& page) const {
        if (!page) return {};
        std::lock_guard<std::mutex> lock(state_mutex);
        std::string joined;
        for (const auto& entry : page->console_logs) {
            if (!joined.empty()) joined.push_back('\n');
            joined += entry;
        }
        return joined;
    }

    void capture_runtime_console(const std::string& page_id,
                                 const std::string& raw) {
        const auto page = find_page(page_id);
        auto event = nlohmann::json::parse(raw, nullptr, false);
        if (!page || event.is_discarded() || !event.is_object()) return;
        const std::string level = event.value("type", "log");
        std::string message;
        if (event.contains("args") && event["args"].is_array()) {
            for (const auto& argument : event["args"]) {
                if (!message.empty()) message.push_back(' ');
                message += cdp_remote_object_text(argument);
            }
        }
        if (message.empty()) message = "console." + level;
        append_console_log(page, "[" + level + "] " + message);
    }

    void capture_runtime_exception(const std::string& page_id,
                                   const std::string& raw) {
        const auto page = find_page(page_id);
        auto event = nlohmann::json::parse(raw, nullptr, false);
        if (!page || event.is_discarded() || !event.is_object()) return;
        const auto details = event.value(
            "exceptionDetails", nlohmann::json::object());
        std::string message = details.value("text", "Uncaught exception");
        if (details.contains("exception") && details["exception"].is_object()) {
            const std::string description =
                cdp_remote_object_text(details["exception"]);
            if (!description.empty() && description != message) {
                message += ": " + description;
            }
        }
        append_console_log(page, "[error] " + message);
    }

    void capture_log_entry(const std::string& page_id,
                           const std::string& raw) {
        const auto page = find_page(page_id);
        auto event = nlohmann::json::parse(raw, nullptr, false);
        if (!page || event.is_discarded() || !event.is_object()) return;
        const auto entry = event.value("entry", nlohmann::json::object());
        if (!entry.is_object()) return;
        const std::string level = entry.value("level", "info");
        std::string message = entry.value("text", "");
        const std::string url = entry.value("url", "");
        if (!url.empty()) message += " (" + url + ")";
        append_console_log(page, "[" + level + "] " + message);
    }

    void install_console_capture(const std::shared_ptr<Page>& page) {
        if (!page || !page->webview) return;
        const auto weak = weak_from_this();
        const std::string page_id = page->id;
        const auto add_receiver = [&](
            const wchar_t* event_name,
            Microsoft::WRL::ComPtr<
                ICoreWebView2DevToolsProtocolEventReceiver>& receiver,
            EventRegistrationToken& token,
            auto capture) {
            if (FAILED(page->webview->GetDevToolsProtocolEventReceiver(
                    event_name, receiver.ReleaseAndGetAddressOf())) ||
                !receiver) {
                return;
            }
            receiver->add_DevToolsProtocolEventReceived(
                Microsoft::WRL::Callback<
                    ICoreWebView2DevToolsProtocolEventReceivedEventHandler>(
                    [weak, page_id, capture](
                        ICoreWebView2*,
                        ICoreWebView2DevToolsProtocolEventReceivedEventArgs*
                            args) -> HRESULT {
                        if (!args) return S_OK;
                        LPWSTR raw = nullptr;
                        args->get_ParameterObjectAsJson(&raw);
                        const std::string text = raw
                            ? acecode::wide_to_utf8(raw) : std::string("{}");
                        ::CoTaskMemFree(raw);
                        if (const auto self = weak.lock()) {
                            (self.get()->*capture)(page_id, text);
                        }
                        return S_OK;
                    }).Get(),
                &token);
        };
        add_receiver(
            L"Runtime.consoleAPICalled",
            page->runtime_console_receiver,
            page->runtime_console_token,
            &Impl::capture_runtime_console);
        add_receiver(
            L"Runtime.exceptionThrown",
            page->runtime_exception_receiver,
            page->runtime_exception_token,
            &Impl::capture_runtime_exception);
        add_receiver(
            L"Log.entryAdded",
            page->log_entry_receiver,
            page->log_entry_token,
            &Impl::capture_log_entry);

        auto enabled = Microsoft::WRL::Callback<
            ICoreWebView2CallDevToolsProtocolMethodCompletedHandler>(
            [page_id](HRESULT result, LPCWSTR) -> HRESULT {
                if (FAILED(result)) {
                    LOG_WARN("[agent-browser] failed to enable console capture for " +
                             page_id + " (" + hresult_text(result) + ")");
                }
                return S_OK;
            });
        page->webview->CallDevToolsProtocolMethod(
            L"Runtime.enable", L"{}", enabled.Get());
        page->webview->CallDevToolsProtocolMethod(
            L"Log.enable", L"{}", enabled.Get());
    }

    void invalidate_element_selection(const std::shared_ptr<Page>& page) {
        if (!page) return;
        AgentBrowserState snapshot;
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            if (page->state.element_selection_active) {
                page->state.element_selection_active = false;
                ++page->element_selection_generation;
                snapshot = page->state;
                changed = true;
            }
        }
        if (changed) emit_state(snapshot);
    }

    void apply_favicon_result(const std::shared_ptr<Page>& page,
                              std::uint64_t generation,
                              std::string favicon) {
        if (!valid_agent_browser_favicon(favicon)) favicon.clear();
        AgentBrowserState snapshot;
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            if (!page || page->closing ||
                page->favicon_generation != generation ||
                page->state.content_state != kAgentBrowserContentStateLive ||
                page->state.favicon == favicon) {
                return;
            }
            page->state.favicon = std::move(favicon);
            snapshot = page->state;
            changed = true;
        }
        if (changed) emit_state(snapshot);
    }

    void refresh_favicon(const std::shared_ptr<Page>& page) {
        if (!page || !page->webview || page->closing) return;
        std::uint64_t generation = 0;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            if (page->state.content_state != kAgentBrowserContentStateLive) {
                return;
            }
            generation = ++page->favicon_generation;
        }

        const auto params = nlohmann::json{
            {"expression", agent_browser_favicon_expression()},
            {"awaitPromise", true},
            {"returnByValue", true},
        }.dump();
        const std::wstring wide_params = acecode::utf8_to_wide(params);
        const auto weak = weak_from_this();
        const std::string page_id = page->id;
        const auto completed = Microsoft::WRL::Callback<
            ICoreWebView2CallDevToolsProtocolMethodCompletedHandler>(
            [weak, page_id, generation](HRESULT result,
                                        LPCWSTR raw_result) -> HRESULT {
                const auto self = weak.lock();
                const auto current = self ? self->find_page(page_id) : nullptr;
                if (!self || !current) return S_OK;
                if (FAILED(result) || !raw_result) {
                    LOG_WARN("[agent-browser] failed to resolve favicon for " +
                             page_id + " (" + hresult_text(result) + ")");
                    return S_OK;
                }
                const auto response = nlohmann::json::parse(
                    acecode::wide_to_utf8(raw_result), nullptr, false);
                if (response.is_discarded() || !response.is_object()) {
                    return S_OK;
                }
                const auto remote = response.value(
                    "result", nlohmann::json::object());
                const std::string favicon =
                    remote.is_object() && remote.contains("value") &&
                        remote["value"].is_string()
                    ? remote["value"].get<std::string>()
                    : std::string();
                self->apply_favicon_result(
                    current, generation, favicon);
                return S_OK;
            });
        const HRESULT dispatched = page->webview->CallDevToolsProtocolMethod(
            L"Runtime.evaluate", wide_params.c_str(), completed.Get());
        if (FAILED(dispatched)) {
            LOG_WARN("[agent-browser] failed to request favicon for " +
                     page_id + " (" + hresult_text(dispatched) + ")");
        }
    }

    void install_events(const std::shared_ptr<Page>& page) {
        const auto weak = weak_from_this();
        const std::string page_id = page->id;
        page->webview->add_NavigationStarting(
            Microsoft::WRL::Callback<ICoreWebView2NavigationStartingEventHandler>(
                [weak, page_id](ICoreWebView2*,
                                 ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                    const auto self = weak.lock();
                    const auto current = self ? self->find_page(page_id) : nullptr;
                    if (!self || !current || !args) return S_OK;
                    LPWSTR raw = nullptr;
                    args->get_Uri(&raw);
                    const std::string uri = raw ? acecode::wide_to_utf8(raw) : "";
                    ::CoTaskMemFree(raw);
                    std::uint64_t navigation_id = 0;
                    args->get_NavigationId(&navigation_id);
                    {
                        std::lock_guard<std::mutex> lock(self->state_mutex);
                        if (current->current_navigation_id != navigation_id &&
                            current->state.content_state !=
                                kAgentBrowserContentStateLoading) {
                            current->content_state_before_navigation =
                                current->state.content_state;
                            current->favicon_before_navigation =
                                current->state.favicon;
                        }
                        current->current_navigation_id = navigation_id;
                        ++current->favicon_generation;
                    }
                    std::string error;
                    if (!normalize_agent_browser_url(uri, &error)) {
                        args->put_Cancel(TRUE);
                        self->update_page(current, [&](AgentBrowserState& state) {
                            state.loading = false;
                            state.error = error;
                        });
                        self->apply_bounds(current);
                        return S_OK;
                    }
                    self->update_page(current, [&](AgentBrowserState& state) {
                        state.loading = true;
                        state.url = uri;
                        state.favicon.clear();
                        state.content_state = kAgentBrowserContentStateLoading;
                        state.failure_kind.clear();
                        state.error.clear();
                    });
                    self->apply_bounds(current);
                    return S_OK;
                }).Get(),
            &page->navigation_starting_token);

        page->webview->add_NavigationCompleted(
            Microsoft::WRL::Callback<ICoreWebView2NavigationCompletedEventHandler>(
                [weak, page_id](ICoreWebView2*,
                                ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                    const auto self = weak.lock();
                    const auto current = self ? self->find_page(page_id) : nullptr;
                    if (!self || !current || !args) return S_OK;
                    BOOL success = FALSE;
                    COREWEBVIEW2_WEB_ERROR_STATUS status =
                        COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
                    std::uint64_t navigation_id = 0;
                    args->get_IsSuccess(&success);
                    args->get_NavigationId(&navigation_id);
                    if (!success) args->get_WebErrorStatus(&status);
                    std::string previous_content_state;
                    std::string previous_favicon;
                    bool process_already_failed = false;
                    {
                        std::lock_guard<std::mutex> lock(self->state_mutex);
                        if (current->current_navigation_id != 0 &&
                            current->current_navigation_id != navigation_id) {
                            return S_OK;
                        }
                        previous_content_state =
                            current->content_state_before_navigation;
                        previous_favicon = current->favicon_before_navigation;
                        process_already_failed =
                            current->state.content_state ==
                                kAgentBrowserContentStateProcessFailed;
                    }
                    if (process_already_failed) return S_OK;
                    LPWSTR raw_source = nullptr;
                    current->webview->get_Source(&raw_source);
                    const std::string source = raw_source
                        ? acecode::wide_to_utf8(raw_source) : "about:blank";
                    ::CoTaskMemFree(raw_source);
                    self->update_page(current, [&](AgentBrowserState& state) {
                        state.loading = false;
                        if (success) {
                            const bool empty = source.empty() ||
                                source == "about:blank";
                            state.url = empty ? "about:blank" : source;
                            state.title = empty
                                ? std::string(kAgentBrowserDefaultTitle)
                                : state.title;
                            state.content_state = empty
                                ? kAgentBrowserContentStateEmpty
                                : kAgentBrowserContentStateLive;
                            state.failure_kind.clear();
                            state.error.clear();
                        } else if (status ==
                                   COREWEBVIEW2_WEB_ERROR_STATUS_OPERATION_CANCELED) {
                            state.content_state = previous_content_state;
                            state.favicon = previous_favicon;
                            state.failure_kind.clear();
                            state.error.clear();
                        } else {
                            state.content_state =
                                kAgentBrowserContentStateNavigationError;
                            state.failure_kind =
                                agent_browser_web_error_kind(status);
                            state.error = "navigation failed (WebView2 status " +
                                std::to_string(static_cast<int>(status)) + ")";
                        }
                    });
                    self->refresh_source_history_title(current);
                    if (success) self->refresh_favicon(current);
                    self->apply_bounds(current);
                    return S_OK;
                }).Get(),
            &page->navigation_completed_token);

        page->webview->add_SourceChanged(
            Microsoft::WRL::Callback<ICoreWebView2SourceChangedEventHandler>(
                [weak, page_id](ICoreWebView2*,
                                ICoreWebView2SourceChangedEventArgs* args) -> HRESULT {
                    if (const auto self = weak.lock()) {
                        const auto current = self->find_page(page_id);
                        BOOL new_document = FALSE;
                        if (args) args->get_IsNewDocument(&new_document);
                        if (new_document) {
                            self->clear_console_logs(current);
                            self->invalidate_element_selection(current);
                        }
                        self->refresh_source_history_title(current);
                    }
                    return S_OK;
                }).Get(),
            &page->source_changed_token);
        page->webview->add_HistoryChanged(
            Microsoft::WRL::Callback<ICoreWebView2HistoryChangedEventHandler>(
                [weak, page_id](ICoreWebView2*, IUnknown*) -> HRESULT {
                    if (const auto self = weak.lock()) {
                        const auto current = self->find_page(page_id);
                        self->refresh_source_history_title(current);
                        self->refresh_favicon(current);
                    }
                    return S_OK;
                }).Get(),
            &page->history_changed_token);
        page->webview->add_DocumentTitleChanged(
            Microsoft::WRL::Callback<ICoreWebView2DocumentTitleChangedEventHandler>(
                [weak, page_id](ICoreWebView2*, IUnknown*) -> HRESULT {
                    if (const auto self = weak.lock()) {
                        const auto current = self->find_page(page_id);
                        self->refresh_source_history_title(current);
                        self->refresh_favicon(current);
                    }
                    return S_OK;
                }).Get(),
            &page->title_changed_token);
        page->webview->add_NewWindowRequested(
            Microsoft::WRL::Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                [weak, page_id](ICoreWebView2*,
                                ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                    const auto self = weak.lock();
                    if (!self || !args) return S_OK;
                    LPWSTR raw = nullptr;
                    args->get_Uri(&raw);
                    const std::string uri = raw ? acecode::wide_to_utf8(raw) : "";
                    ::CoTaskMemFree(raw);
                    args->put_Handled(TRUE);
                    std::string ignored;
                    self->navigate_on_ui(page_id, uri, &ignored);
                    return S_OK;
                }).Get(),
            &page->new_window_token);
        page->webview->add_ProcessFailed(
            Microsoft::WRL::Callback<ICoreWebView2ProcessFailedEventHandler>(
                [weak, page_id](ICoreWebView2*,
                                ICoreWebView2ProcessFailedEventArgs* args) -> HRESULT {
                    const auto self = weak.lock();
                    const auto current = self ? self->find_page(page_id) : nullptr;
                    if (!self || !current || !args) return S_OK;

                    COREWEBVIEW2_PROCESS_FAILED_KIND kind =
                        COREWEBVIEW2_PROCESS_FAILED_KIND_UNKNOWN_PROCESS_EXITED;
                    COREWEBVIEW2_PROCESS_FAILED_REASON reason =
                        COREWEBVIEW2_PROCESS_FAILED_REASON_UNEXPECTED;
                    int exit_code = 0;
                    args->get_ProcessFailedKind(&kind);
                    Microsoft::WRL::ComPtr<
                        ICoreWebView2ProcessFailedEventArgs2> details;
                    if (SUCCEEDED(args->QueryInterface(IID_PPV_ARGS(&details))) &&
                        details) {
                        details->get_Reason(&reason);
                        details->get_ExitCode(&exit_code);
                    }
                    const std::string failure_kind =
                        agent_browser_process_failure_kind(kind, reason);
                    const std::string technical =
                        "process failed (kind " +
                        std::to_string(static_cast<int>(kind)) + ", reason " +
                        std::to_string(static_cast<int>(reason)) + ", exit " +
                        std::to_string(exit_code) + ")";
                    self->append_console_log(
                        current, "[browser] " + technical);
                    if (failure_kind.empty()) return S_OK;

                    self->invalidate_element_selection(current);
                    self->update_page(current, [&](AgentBrowserState& state) {
                        state.loading = false;
                        state.content_state =
                            kAgentBrowserContentStateProcessFailed;
                        state.failure_kind = failure_kind;
                        state.error = technical;
                    });
                    self->apply_bounds(current);
                    return S_OK;
                }).Get(),
            &page->process_failed_token);
    }

    void refresh_source_history_title(const std::shared_ptr<Page>& page) {
        if (!page || !page->webview || page->closing) return;
        LPWSTR raw_source = nullptr;
        LPWSTR raw_title = nullptr;
        BOOL can_back = FALSE;
        BOOL can_forward = FALSE;
        page->webview->get_Source(&raw_source);
        page->webview->get_DocumentTitle(&raw_title);
        page->webview->get_CanGoBack(&can_back);
        page->webview->get_CanGoForward(&can_forward);
        const std::string source = raw_source
            ? acecode::wide_to_utf8(raw_source) : "about:blank";
        const std::string title = agent_browser_title_or_default(
            raw_title ? acecode::wide_to_utf8(raw_title) : "");
        ::CoTaskMemFree(raw_source);
        ::CoTaskMemFree(raw_title);
        update_page(page, [&](AgentBrowserState& state) {
            if (state.content_state == kAgentBrowserContentStateLive) {
                state.url = source;
                state.title = title;
            }
            state.can_go_back = can_back != FALSE;
            state.can_go_forward = can_forward != FALSE;
        });
    }

    void remove_events(const std::shared_ptr<Page>& page) {
        if (!page || !page->webview) return;
        page->webview->remove_NavigationStarting(page->navigation_starting_token);
        page->webview->remove_NavigationCompleted(page->navigation_completed_token);
        page->webview->remove_SourceChanged(page->source_changed_token);
        page->webview->remove_HistoryChanged(page->history_changed_token);
        page->webview->remove_DocumentTitleChanged(page->title_changed_token);
        page->webview->remove_NewWindowRequested(page->new_window_token);
        page->webview->remove_ProcessFailed(page->process_failed_token);
        if (page->runtime_console_receiver) {
            page->runtime_console_receiver->remove_DevToolsProtocolEventReceived(
                page->runtime_console_token);
            page->runtime_console_receiver.Reset();
        }
        if (page->runtime_exception_receiver) {
            page->runtime_exception_receiver->remove_DevToolsProtocolEventReceived(
                page->runtime_exception_token);
            page->runtime_exception_receiver.Reset();
        }
        if (page->log_entry_receiver) {
            page->log_entry_receiver->remove_DevToolsProtocolEventReceived(
                page->log_entry_token);
            page->log_entry_receiver.Reset();
        }
    }

    void teardown_page(const std::shared_ptr<Page>& page) {
        if (!page) return;
        remove_events(page);
        if (page->controller) {
            page->controller->put_IsVisible(FALSE);
            page->controller->Close();
        }
        page->webview.Reset();
        page->controller.Reset();
    }

    bool select_page_on_ui(const std::string& page_id, std::string* error) {
        auto selected = find_page(page_id);
        if (!selected || selected->closing) {
            assign_error(error, "Agent Browser page was not found");
            return false;
        }
        std::vector<AgentBrowserState> changed;
        std::vector<std::shared_ptr<Page>> all_pages;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            active_page = page_id;
            for (const auto& [id, page] : pages) {
                const bool active = id == page_id;
                if (page->state.active != active) {
                    page->state.active = active;
                    changed.push_back(page->state);
                }
                all_pages.push_back(page);
            }
        }
        for (const auto& snapshot : changed) emit_state(snapshot);
        for (const auto& page : all_pages) apply_bounds(page);
        return true;
    }

    bool close_page_on_ui(const std::string& requested_page,
                          std::string* closed_page_id,
                          std::string* error) {
        const std::string page_id = requested_page.empty()
            ? active_page_id() : requested_page;
        auto page = find_page(page_id);
        if (!page) {
            assign_error(error, "Agent Browser page was not found");
            return false;
        }
        page->closing = true;
        teardown_page(page);
        std::vector<QueuedCdpCall> queued;
        queued.swap(page->queued_cdp);
        std::string next_active;
        bool closed_active = false;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            pages.erase(page_id);
            page_order.erase(
                std::remove(page_order.begin(), page_order.end(), page_id),
                page_order.end());
            if (active_page == page_id) {
                closed_active = true;
                active_page.clear();
                if (!page_order.empty()) next_active = page_order.back();
            }
            page->state.ready = false;
            page->state.loading = false;
            page->state.visible = false;
            page->state.active = false;
            page->state.closed = true;
        }
        emit_state(page->state);
        for (auto& call : queued) {
            finish_proxy_call(call.pending,
                              {{"ok", false},
                               {"page_id", page_id},
                               {"error", "Agent Browser page was closed"}});
        }
        if (closed_active) {
            if (!next_active.empty()) {
                std::string ignored;
                select_page_on_ui(next_active, &ignored);
            } else {
                hide_agent_browser_widget(browser_widget);
            }
        }
        if (closed_page_id) *closed_page_id = page_id;
        LOG_INFO("[agent-browser] page closed: " + page_id);
        return true;
    }

    void apply_bounds(const std::shared_ptr<Page>& page) {
        if (!page) return;
        const int width = (std::max)(0, page->requested_bounds.width);
        const int height = (std::max)(0, page->requested_bounds.height);
        const auto parent = static_cast<HWND>(parent_window);
        const bool parent_visible = parent && ::IsWindow(parent) &&
                                    ::IsWindowVisible(parent) &&
                                    !::IsIconic(parent);
        const bool requested_show = page->controller && page->state.active &&
                                    page->state.content_state ==
                                        kAgentBrowserContentStateLive &&
                                    page->requested_bounds.visible &&
                                    parent_surface_visible &&
                                    parent_visible &&
                                    width > 0 && height > 0;
        bool show = false;
        if (page->controller) {
            const bool positioned = requested_show && browser_widget &&
                ::SetWindowPos(browser_widget,
                               HWND_TOP,
                               page->requested_bounds.x,
                               page->requested_bounds.y,
                               width,
                               height,
                               SWP_NOACTIVATE | SWP_SHOWWINDOW) != FALSE;
            const bool region_applied = positioned &&
                apply_agent_browser_widget_region(
                    browser_widget,
                    width,
                    height,
                    page->requested_bounds.occlusion_rects);
            if (region_applied) {
                const RECT controller_bounds{0, 0, width, height};
                const HRESULT bounds_result =
                    page->controller->put_Bounds(controller_bounds);
                page->controller->NotifyParentWindowPositionChanged();
                const HRESULT visible_result =
                    page->controller->put_IsVisible(TRUE);
                show = SUCCEEDED(bounds_result) && SUCCEEDED(visible_result);
                if (!show) {
                    LOG_WARN("[agent-browser] failed to show native page " +
                             page->id + " (bounds=" +
                             hresult_text(bounds_result) + ", visible=" +
                             hresult_text(visible_result) + ")");
                }
            } else {
                page->controller->put_IsVisible(FALSE);
                if (positioned) {
                    LOG_WARN("[agent-browser] failed to apply native occlusion "
                             "region for page " + page->id);
                }
            }
        }
        if (page->state.active && !show) {
            hide_agent_browser_widget(browser_widget);
        }
        if (page->state.visible != show) {
            update_page(page, [&](AgentBrowserState& state) {
                state.visible = show;
            });
        }
    }

    bool navigate_on_ui(const std::string& page_id,
                        const std::string& input,
                        std::string* error) {
        auto page = find_page(page_id);
        if (!page || !page->webview) {
            assign_error(error, "Agent Browser page is still starting");
            return false;
        }
        std::string normalize_error;
        const auto url = normalize_agent_browser_url(input, &normalize_error);
        if (!url) {
            assign_error(error, normalize_error);
            return false;
        }
        const HRESULT result = page->webview->Navigate(
            acecode::utf8_to_wide(*url).c_str());
        if (FAILED(result)) {
            assign_error(error, "WebView2 navigation failed (" +
                                    hresult_text(result) + ")");
            return false;
        }
        return true;
    }

    bool set_shared_with_agent_on_ui(const std::string& page_id,
                                     bool shared,
                                     std::string* error) {
        auto page = find_page(page_id);
        if (!page || page->closing) {
            assign_error(error, "Agent Browser page was not found");
            return false;
        }
        update_page(page, [shared](AgentBrowserState& state) {
            state.shared_with_agent = shared;
        });
        return true;
    }

    void finish_element_selection(const std::string& page_id,
                                  std::uint64_t generation,
                                  HRESULT result,
                                  LPCWSTR raw) {
        const auto page = find_page(page_id);
        if (!page) return;
        nlohmann::json selected;
        if (SUCCEEDED(result) && raw) {
            auto response = nlohmann::json::parse(
                acecode::wide_to_utf8(raw), nullptr, false);
            if (!response.is_discarded() && response.is_object()) {
                const auto remote = response.value(
                    "result", nlohmann::json::object());
                if (remote.is_object() && remote.contains("value") &&
                    remote["value"].is_object()) {
                    const auto& value = remote["value"];
                    if (!value.value("cancelled", true) &&
                        value.contains("element") &&
                        value["element"].is_object()) {
                        selected = value["element"];
                    }
                }
            }
        }

        AgentBrowserState snapshot;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            if (page->closing ||
                page->element_selection_generation != generation) {
                return;
            }
            page->state.element_selection_active = false;
            if (selected.is_object()) {
                ++page->state.element_selection_serial;
            }
            snapshot = page->state;
        }
        if (selected.is_object()) {
            snapshot.selected_element_json = selected.dump();
        }
        emit_state(snapshot);
    }

    bool toggle_element_selection_on_ui(const std::string& page_id,
                                        std::string* error) {
        auto page = find_page(page_id);
        if (!page || !page->webview || page->closing) {
            assign_error(error, "Agent Browser page is still starting");
            return false;
        }
        bool active = false;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            active = page->state.element_selection_active;
        }
        if (active) {
            const std::string cancel_expression =
                "(() => { const value = globalThis['" +
                std::string(kAgentBrowserElementPickerKey) +
                "']; if (value && typeof value.cancel === 'function') "
                "value.cancel(); return true; })()";
            const nlohmann::json params{
                {"expression", cancel_expression},
                {"returnByValue", true},
                {"userGesture", true},
            };
            const std::wstring wide_params = acecode::utf8_to_wide(params.dump());
            auto ignored = Microsoft::WRL::Callback<
                ICoreWebView2CallDevToolsProtocolMethodCompletedHandler>(
                [](HRESULT, LPCWSTR) -> HRESULT { return S_OK; });
            const HRESULT dispatched = page->webview->CallDevToolsProtocolMethod(
                L"Runtime.evaluate", wide_params.c_str(), ignored.Get());
            if (FAILED(dispatched)) {
                assign_error(error, "failed to cancel element selection (" +
                                        hresult_text(dispatched) + ")");
                return false;
            }
            return true;
        }

        std::uint64_t generation = 0;
        AgentBrowserState snapshot;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            generation = ++page->element_selection_generation;
            page->state.element_selection_active = true;
            snapshot = page->state;
        }
        emit_state(snapshot);

        const nlohmann::json params{
            {"expression", agent_browser_element_picker_expression()},
            {"awaitPromise", true},
            {"returnByValue", true},
            {"userGesture", true},
        };
        const std::wstring wide_params = acecode::utf8_to_wide(params.dump());
        const auto weak = weak_from_this();
        auto completed = Microsoft::WRL::Callback<
            ICoreWebView2CallDevToolsProtocolMethodCompletedHandler>(
            [weak, page_id, generation](HRESULT result, LPCWSTR raw) -> HRESULT {
                if (const auto self = weak.lock()) {
                    self->finish_element_selection(
                        page_id, generation, result, raw);
                }
                return S_OK;
            });
        const HRESULT dispatched = page->webview->CallDevToolsProtocolMethod(
            L"Runtime.evaluate", wide_params.c_str(), completed.Get());
        if (FAILED(dispatched)) {
            finish_element_selection(page_id, generation, dispatched, nullptr);
            assign_error(error, "failed to start element selection (" +
                                    hresult_text(dispatched) + ")");
            return false;
        }
        return true;
    }

    bool open_developer_tools_on_ui(const std::string& page_id,
                                    std::string* error) {
        auto page = find_page(page_id);
        if (!page || !page->webview || page->closing) {
            assign_error(error, "Agent Browser page is still starting");
            return false;
        }
        const HRESULT result = page->webview->OpenDevToolsWindow();
        if (FAILED(result)) {
            assign_error(error, "failed to open WebView2 developer tools (" +
                                    hresult_text(result) + ")");
            return false;
        }
        return true;
    }

    void call_cdp_on_ui(
        const std::string& requested_page,
        const std::string& method,
        const nlohmann::json& params,
        const std::shared_ptr<PendingProxyCall>& pending) {
        std::string page_id = requested_page;
        if (page_id.empty()) page_id = active_page_id();
        if (page_id.empty()) page_id = create_page_on_ui(true);
        auto page = find_page(page_id);
        if (!page || page->closing) {
            finish_proxy_call(pending,
                              {{"ok", false},
                               {"page_id", page_id},
                               {"error", "Agent Browser page was not found"}});
            return;
        }
        if (!require_agent_shared_page(page, page_id, pending)) return;
        if (!page->webview) {
            std::string host_error;
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                host_error = host_state.error;
            }
            if (!host_error.empty()) {
                finish_proxy_call(pending,
                                  {{"ok", false},
                                   {"page_id", page_id},
                                   {"error", host_error}});
                return;
            }
            page->queued_cdp.push_back({method, params, pending});
            if (environment) begin_create_controller(page);
            return;
        }
        const std::wstring wide_method = acecode::utf8_to_wide(method);
        const std::wstring wide_params = acecode::utf8_to_wide(
            (params.is_object() ? params : nlohmann::json::object()).dump());
        auto completed = Microsoft::WRL::Callback<
            ICoreWebView2CallDevToolsProtocolMethodCompletedHandler>(
            [pending, page_id](HRESULT result, LPCWSTR raw) -> HRESULT {
                if (FAILED(result)) {
                    finish_proxy_call(
                        pending,
                        {{"ok", false},
                         {"page_id", page_id},
                         {"error", "WebView2 CDP call failed (" +
                                       hresult_text(result) + ")"}});
                    return S_OK;
                }
                const std::string text = raw
                    ? acecode::wide_to_utf8(raw) : std::string("{}");
                auto value = nlohmann::json::parse(text, nullptr, false);
                if (value.is_discarded()) {
                    finish_proxy_call(
                        pending,
                        {{"ok", false},
                         {"page_id", page_id},
                         {"error", "WebView2 returned malformed CDP JSON"}});
                    return S_OK;
                }
                finish_proxy_call(
                    pending,
                    {{"ok", true},
                     {"page_id", page_id},
                     {"result", std::move(value)}});
                return S_OK;
            });
        const HRESULT started = page->webview->CallDevToolsProtocolMethod(
            wide_method.c_str(), wide_params.c_str(), completed.Get());
        if (FAILED(started)) {
            finish_proxy_call(
                pending,
                {{"ok", false},
                 {"page_id", page_id},
                 {"error", "failed to dispatch WebView2 CDP call (" +
                               hresult_text(started) + ")"}});
        }
    }

    void execute_proxy_request_on_ui(
        const nlohmann::json& request,
        const std::shared_ptr<PendingProxyCall>& pending) {
        const std::string operation = request.value("operation", "cdp");
        const std::string requested_page = request.value("page_id", "");
        if (operation == "create_page") {
            const std::string page_id = create_page_on_ui(true);
            finish_proxy_call(pending,
                              {{"ok", true},
                               {"page_id", page_id},
                               {"result", {{"page_id", page_id}}}});
            return;
        }
        if (operation == "claim_page") {
            std::string page_id = requested_page.empty()
                ? active_page_id() : requested_page;
            if (page_id.empty()) page_id = create_page_on_ui(true);
            const auto page = find_page(page_id);
            if (!page) {
                finish_proxy_call(pending,
                                  {{"ok", false},
                                   {"page_id", page_id},
                                   {"error", "Agent Browser page was not found"}});
            } else if (require_agent_shared_page(page, page_id, pending)) {
                finish_proxy_call(pending,
                                  {{"ok", true},
                                   {"page_id", page_id},
                                   {"result", {{"page_id", page_id}}}});
            }
            return;
        }
        if (operation == "close_page") {
            const std::string page_id = requested_page.empty()
                ? active_page_id() : requested_page;
            const auto page = find_page(page_id);
            if (!page) {
                finish_proxy_call(pending,
                                  {{"ok", false},
                                   {"page_id", page_id},
                                   {"error", "Agent Browser page was not found"}});
                return;
            }
            if (!require_agent_shared_page(page, page_id, pending)) return;
            std::string closed_page;
            std::string error;
            if (!close_page_on_ui(requested_page, &closed_page, &error)) {
                finish_proxy_call(pending,
                                  {{"ok", false},
                                   {"page_id", requested_page},
                                   {"error", error}});
            } else {
                finish_proxy_call(pending,
                                  {{"ok", true},
                                   {"page_id", closed_page},
                                   {"result", {{"closed", true},
                                               {"page_id", closed_page}}}});
            }
            return;
        }
        if (operation == "select_page") {
            const auto page = find_page(requested_page);
            if (!page) {
                finish_proxy_call(pending,
                                  {{"ok", false},
                                   {"page_id", requested_page},
                                   {"error", "Agent Browser page was not found"}});
                return;
            }
            if (!require_agent_shared_page(page, requested_page, pending)) return;
            std::string error;
            if (!select_page_on_ui(requested_page, &error)) {
                finish_proxy_call(pending,
                                  {{"ok", false},
                                   {"page_id", requested_page},
                                   {"error", error}});
            } else {
                finish_proxy_call(pending,
                                  {{"ok", true},
                                   {"page_id", requested_page},
                                   {"result", {{"page_id", requested_page}}}});
            }
            return;
        }
        call_cdp_on_ui(
            requested_page,
            request.value("method", ""),
            request.contains("params") && request["params"].is_object()
                ? request["params"] : nlohmann::json::object(),
            pending);
    }

    nlohmann::json execute_proxy_request(const nlohmann::json& request) {
        if (!request.is_object()) {
            return {{"ok", false}, {"error", "proxy request must be an object"}};
        }
        if (!request.contains("auth_token") ||
            !request["auth_token"].is_string() ||
            request["auth_token"].get_ref<const std::string&>() !=
                proxy_auth_token) {
            return {{"ok", false},
                    {"error", "Agent Browser proxy authentication failed"}};
        }
        const std::string operation = request.value("operation", "cdp");
        if (operation != "cdp" && operation != "create_page" &&
            operation != "claim_page" &&
            operation != "close_page" && operation != "select_page") {
            return {{"ok", false},
                    {"error", "Agent Browser proxy operation is invalid"}};
        }
        if (operation == "cdp") {
            if (!request.contains("method") || !request["method"].is_string()) {
                return {{"ok", false},
                        {"error", "Agent Browser CDP method is invalid"}};
            }
            const std::string method = request["method"].get<std::string>();
            if (method.empty() || method.size() > 256) {
                return {{"ok", false},
                        {"error", "Agent Browser CDP method is invalid"}};
            }
        }
        const int requested_timeout = request.contains("timeout_ms") &&
                request["timeout_ms"].is_number_integer()
            ? request["timeout_ms"].get<int>() : 15000;
        const int timeout_ms = (std::max)(
            100, (std::min)(120000, requested_timeout));
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeout_ms);
        auto pending = std::make_shared<PendingProxyCall>();
        const auto weak = weak_from_this();
        try {
            dispatch_handler([weak, request, pending] {
                if (const auto self = weak.lock()) {
                    self->execute_proxy_request_on_ui(request, pending);
                } else {
                    finish_proxy_call(
                        pending,
                        {{"ok", false}, {"error", "Agent Browser stopped"}});
                }
            });
        } catch (const std::exception& error) {
            return {{"ok", false},
                    {"error", std::string("Agent Browser dispatch failed: ") +
                                  error.what()}};
        }

        std::unique_lock<std::mutex> lock(pending->mutex);
        while (!pending->completed && !proxy_stopping.load() &&
               std::chrono::steady_clock::now() < deadline) {
            pending->ready.wait_for(lock, std::chrono::milliseconds(50));
        }
        if (pending->completed) return pending->response;
        return {{"ok", false},
                {"page_id", request.value("page_id", "")},
                {"error", proxy_stopping.load()
                    ? "Agent Browser stopped"
                    : "WebView2 CDP call timed out"}};
    }

    void handle_proxy_connection(HANDLE pipe) {
        const auto request_deadline = std::chrono::steady_clock::now() +
                                      std::chrono::seconds(5);
        std::uint32_t request_size = 0;
        if (!pipe_transfer(pipe, &request_size, sizeof(request_size), false,
                           proxy_stopping, request_deadline) ||
            request_size == 0 ||
            request_size > kAgentBrowserProxyMaxRequestBytes) {
            return;
        }
        std::string payload(request_size, '\0');
        if (!pipe_transfer(pipe, payload.data(), payload.size(), false,
                           proxy_stopping, request_deadline)) {
            return;
        }
        const auto request = nlohmann::json::parse(payload, nullptr, false);
        std::string response_text = execute_proxy_request(request).dump();
        if (response_text.size() > kAgentBrowserProxyMaxResponseBytes) return;
        std::uint32_t response_size =
            static_cast<std::uint32_t>(response_text.size());
        const auto response_deadline = std::chrono::steady_clock::now() +
                                       std::chrono::seconds(10);
        if (!pipe_transfer(pipe, &response_size,
                           sizeof(response_size), true,
                           proxy_stopping, response_deadline)) {
            return;
        }
        if (!pipe_transfer(pipe, response_text.data(),
                           response_text.size(), true,
                           proxy_stopping, response_deadline)) {
            return;
        }

        // DisconnectNamedPipe discards unread buffered data. Wait for a
        // bounded acknowledgement before proxy_loop disconnects the client.
        std::uint8_t acknowledgement = 0;
        const auto acknowledgement_deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(2);
        if (!pipe_transfer(pipe, &acknowledgement,
                           sizeof(acknowledgement), false,
                           proxy_stopping, acknowledgement_deadline) ||
            acknowledgement != kAgentBrowserProxyResponseAck) {
            return;
        }
    }

    void report_proxy_failure(const std::string& message) {
        const auto weak = weak_from_this();
        try {
            dispatch_handler([weak, message] {
                if (const auto self = weak.lock()) self->fail(message);
            });
        } catch (const std::exception& error) {
            LOG_ERROR("[agent-browser] failed to dispatch proxy error: " +
                      std::string(error.what()));
        }
    }

    void proxy_loop() {
        const std::wstring pipe_name = acecode::utf8_to_wide(proxy_pipe_name);
        while (!proxy_stopping.load()) {
            HANDLE pipe = ::CreateNamedPipeW(
                pipe_name.c_str(),
                PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
                    PIPE_REJECT_REMOTE_CLIENTS,
                1,
                64u * 1024u,
                64u * 1024u,
                0,
                nullptr);
            if (pipe == INVALID_HANDLE_VALUE) {
                if (!proxy_stopping.load()) {
                    report_proxy_failure(
                        "failed to create Agent Browser proxy pipe (" +
                        std::to_string(::GetLastError()) + ")");
                }
                return;
            }
            if (connect_pipe(pipe, proxy_stopping)) {
                handle_proxy_connection(pipe);
                ::DisconnectNamedPipe(pipe);
            }
            ::CloseHandle(pipe);
        }
    }

#endif
};

AgentBrowserHost::AgentBrowserHost(void* parent_window,
                                   std::int64_t desktop_pid,
                                   std::string desktop_instance_id,
                                   StateHandler state_handler,
                                   DispatchHandler dispatch_handler,
                                   std::string acecode_dir)
    : impl_(std::make_shared<Impl>(parent_window,
                                  desktop_pid,
                                  std::move(desktop_instance_id),
                                  std::move(state_handler),
                                  std::move(dispatch_handler),
                                  std::move(acecode_dir))) {
#ifdef _WIN32
    impl_->start();
#endif
}

AgentBrowserHost::~AgentBrowserHost() = default;

bool AgentBrowserHost::supported() const {
    return impl_ && impl_->state().supported;
}

AgentBrowserState AgentBrowserHost::state(const std::string& page_id) const {
    return impl_ ? impl_->state(page_id) : AgentBrowserState{};
}

std::vector<AgentBrowserState> AgentBrowserHost::states() const {
    return impl_ ? impl_->states() : std::vector<AgentBrowserState>{};
}

std::string AgentBrowserHost::active_page_id() const {
    return impl_ ? impl_->active_page_id() : std::string{};
}

std::string AgentBrowserHost::create_page(std::string* error) {
#ifdef _WIN32
    if (impl_ && impl_->state().supported) return impl_->create_page_on_ui(false);
#endif
    assign_error(error, "Agent Browser is unavailable on this platform");
    return {};
}

bool AgentBrowserHost::close_page(const std::string& page_id,
                                  std::string* error) {
#ifdef _WIN32
    std::string ignored;
    return impl_ && impl_->close_page_on_ui(page_id, &ignored, error);
#else
    (void)page_id;
    assign_error(error, "Agent Browser is unavailable on this platform");
    return false;
#endif
}

bool AgentBrowserHost::select_page(const std::string& page_id,
                                   std::string* error) {
#ifdef _WIN32
    return impl_ && impl_->select_page_on_ui(page_id, error);
#else
    (void)page_id;
    assign_error(error, "Agent Browser is unavailable on this platform");
    return false;
#endif
}

bool AgentBrowserHost::set_bounds(const std::string& page_id,
                                  const AgentBrowserBounds& bounds,
                                  std::string* error) {
    if (!impl_ || !impl_->state().supported) {
        assign_error(error, "Agent Browser is unavailable on this platform");
        return false;
    }
    if (bounds.x < 0 || bounds.y < 0 || bounds.width < 0 || bounds.height < 0 ||
        bounds.width > 32768 || bounds.height > 32768) {
        assign_error(error, "Agent Browser bounds are invalid");
        return false;
    }
    if (bounds.occlusion_rects.size() > kAgentBrowserMaxOcclusionRects) {
        assign_error(error, "Agent Browser has too many occlusion rectangles");
        return false;
    }
    for (const auto& rect : bounds.occlusion_rects) {
        if (rect.x < 0 || rect.y < 0 || rect.width <= 0 || rect.height <= 0 ||
            rect.x > 32768 || rect.y > 32768 ||
            rect.width > 32768 || rect.height > 32768) {
            assign_error(error, "Agent Browser occlusion rectangle is invalid");
            return false;
        }
    }
#ifdef _WIN32
    auto page = impl_->find_page(page_id);
    if (!page) {
        assign_error(error, "Agent Browser page was not found");
        return false;
    }
    if (bounds.layout_revision != 0 &&
        page->requested_bounds.layout_revision != 0 &&
        bounds.layout_revision < page->requested_bounds.layout_revision) {
        return true;
    }
    page->requested_bounds = bounds;
    impl_->apply_bounds(page);
#endif
    return true;
}

bool AgentBrowserHost::navigate(const std::string& page_id,
                                const std::string& input,
                                std::string* error) {
#ifdef _WIN32
    return impl_ && impl_->navigate_on_ui(page_id, input, error);
#else
    (void)page_id;
    (void)input;
    assign_error(error, "Agent Browser is unavailable on this platform");
    return false;
#endif
}

bool AgentBrowserHost::go_back(const std::string& page_id,
                               std::string* error) {
#ifdef _WIN32
    auto page = impl_ ? impl_->find_page(page_id) : nullptr;
    if (page && page->webview && page->state.can_go_back &&
        SUCCEEDED(page->webview->GoBack())) return true;
    assign_error(error, "Agent Browser cannot go back");
#else
    (void)page_id;
    assign_error(error, "Agent Browser is unavailable on this platform");
#endif
    return false;
}

bool AgentBrowserHost::go_forward(const std::string& page_id,
                                  std::string* error) {
#ifdef _WIN32
    auto page = impl_ ? impl_->find_page(page_id) : nullptr;
    if (page && page->webview && page->state.can_go_forward &&
        SUCCEEDED(page->webview->GoForward())) return true;
    assign_error(error, "Agent Browser cannot go forward");
#else
    (void)page_id;
    assign_error(error, "Agent Browser is unavailable on this platform");
#endif
    return false;
}

bool AgentBrowserHost::reload(const std::string& page_id,
                              std::string* error) {
#ifdef _WIN32
    auto page = impl_ ? impl_->find_page(page_id) : nullptr;
    if (page && page->webview && SUCCEEDED(page->webview->Reload())) return true;
    assign_error(error, "Agent Browser page is still starting");
#else
    (void)page_id;
    assign_error(error, "Agent Browser is unavailable on this platform");
#endif
    return false;
}

bool AgentBrowserHost::focus(const std::string& page_id,
                             std::string* error) {
#ifdef _WIN32
    auto page = impl_ ? impl_->find_page(page_id) : nullptr;
    if (page && page->controller &&
        SUCCEEDED(page->controller->MoveFocus(
            COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC))) return true;
    assign_error(error, "Agent Browser page is still starting");
#else
    (void)page_id;
    assign_error(error, "Agent Browser is unavailable on this platform");
#endif
    return false;
}

bool AgentBrowserHost::set_shared_with_agent(const std::string& page_id,
                                              bool shared,
                                              std::string* error) {
#ifdef _WIN32
    return impl_ && impl_->set_shared_with_agent_on_ui(page_id, shared, error);
#else
    (void)page_id;
    (void)shared;
    assign_error(error, "Agent Browser is unavailable on this platform");
    return false;
#endif
}

bool AgentBrowserHost::toggle_element_selection(const std::string& page_id,
                                                 std::string* error) {
#ifdef _WIN32
    return impl_ && impl_->toggle_element_selection_on_ui(page_id, error);
#else
    (void)page_id;
    assign_error(error, "Agent Browser is unavailable on this platform");
    return false;
#endif
}

std::string AgentBrowserHost::console_logs(const std::string& page_id,
                                            std::string* error) const {
#ifdef _WIN32
    const auto page = impl_ ? impl_->find_page(page_id) : nullptr;
    if (page && !page->closing) return impl_->console_logs_for_page(page);
    assign_error(error, "Agent Browser page was not found");
#else
    (void)page_id;
    assign_error(error, "Agent Browser is unavailable on this platform");
#endif
    return {};
}

bool AgentBrowserHost::open_developer_tools(const std::string& page_id,
                                             std::string* error) {
#ifdef _WIN32
    return impl_ && impl_->open_developer_tools_on_ui(page_id, error);
#else
    (void)page_id;
    assign_error(error, "Agent Browser is unavailable on this platform");
    return false;
#endif
}

void AgentBrowserHost::refresh_layout() {
    if (!impl_) return;
#ifdef _WIN32
    for (const auto& state : impl_->states()) {
        if (auto page = impl_->find_page(state.page_id)) {
            impl_->apply_bounds(page);
        }
    }
#endif
}

void AgentBrowserHost::set_parent_visible(bool visible) {
    if (!impl_) return;
    impl_->parent_surface_visible = visible;
    refresh_layout();
}

void AgentBrowserHost::hide(const std::string& page_id) {
    if (!impl_) return;
#ifdef _WIN32
    if (!page_id.empty()) {
        if (auto page = impl_->find_page(page_id)) {
            page->requested_bounds.visible = false;
            impl_->apply_bounds(page);
        }
        return;
    }
    for (const auto& state : impl_->states()) {
        if (auto page = impl_->find_page(state.page_id)) {
            page->requested_bounds.visible = false;
            impl_->apply_bounds(page);
        }
    }
#else
    (void)page_id;
#endif
}

} // namespace acecode::desktop
