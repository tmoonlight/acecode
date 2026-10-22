#include "agent_browser_host.hpp"

#include "agent_browser_cookie_jars.hpp"
#include "agent_browser_navigation_state.hpp"
#include "ipc/agent_browser_runtime.hpp"
#include "ipc/agent_browser_store_state.hpp"
#include "window_chrome.hpp"

#include "platform/process/os_process.hpp"
#include "utils/encoding.hpp"
#include "utils/logger.hpp"
#include "utils/token.hpp"
#include "utils/utf8_path.hpp"

#include <atomic>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include "utils/joining_thread.hpp"
#include <unordered_map>
#include <unordered_set>
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
#  include <shellapi.h>
#  include <wrl.h>
#  include <WebView2.h>
#  include <WebView2EnvironmentOptions.h>
#  include <webview/webview.h>
#endif


#include "agent_browser_windows_helpers.hpp"

#ifdef _WIN32
namespace acecode::desktop::agent_browser_windows {


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

// 可以交给操作系统打开的外部 scheme。http/https/file/about 由 WebView 自己承载;
// javascript/data/blob 不代表外部应用,edge/devtools 是引擎内部页面。
bool agent_browser_external_handoff_candidate(const std::string& uri) {
    const auto colon = uri.find(':');
    if (colon == std::string::npos || colon == 0) return false;
    std::string scheme = uri.substr(0, colon);
    std::transform(scheme.begin(), scheme.end(), scheme.begin(),
                   [](unsigned char ch) {
                       return static_cast<char>(std::tolower(ch));
                   });
    if (std::isalpha(static_cast<unsigned char>(scheme.front())) == 0) {
        return false;
    }
    static const char* const blocked[] = {
        "http", "https", "file", "about", "javascript", "data", "blob",
        "edge", "devtools", "ws", "wss",
    };
    for (const char* name : blocked) {
        if (scheme == name) return false;
    }
    return true;
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

// 页面给的 bounds 是主 WebView 视口坐标,而 Agent Browser 子窗口挂在宿主窗口
// 上。两者原点通常重合;Win10 非最大化时主 WebView 下移了 1px 给自绘顶边线
// (web_host.cpp resize_webview_widget),这里要把偏移加回来,否则整块往上错位。
POINT main_web_content_origin(HWND parent) {
    POINT origin{0, 0};
    HWND widget = ::FindWindowExW(parent, nullptr, kWebViewWidgetClassName, nullptr);
    RECT rect{};
    if (!widget || !::GetWindowRect(widget, &rect)) return origin;
    origin.x = rect.left;
    origin.y = rect.top;
    ::MapWindowPoints(HWND_DESKTOP, parent, &origin, 1);
    return origin;
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

bool remove_session_profile_directory(
    const std::filesystem::path& user_data_path,
    const std::string& session_id) {
    const auto directory = user_data_path / "EBWebView" /
        acecode::path_from_utf8(agent_browser_session_profile_directory_name(session_id));
    std::error_code ec;
    std::filesystem::remove_all(directory, ec);
    if (ec) {
        LOG_WARN("[agent-browser] failed to remove session store " +
                 acecode::path_to_utf8(directory) + ": " + ec.message());
        return false;
    }
    return true;
}

HRESULT create_controller_for_store(
    const Microsoft::WRL::ComPtr<ICoreWebView2Environment>& environment,
    HWND browser_widget,
    const AgentBrowserCookieStoreKey& key,
    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler* handler) {
    if (!environment) return E_UNEXPECTED;
    if (key.kind == AgentBrowserCookieStoreKind::Shared) {
        return environment->CreateCoreWebView2Controller(browser_widget, handler);
    }
    Microsoft::WRL::ComPtr<ICoreWebView2Environment10> environment10;
    if (FAILED(environment.As(&environment10)) || !environment10) {
        return E_NOINTERFACE;
    }
    Microsoft::WRL::ComPtr<ICoreWebView2ControllerOptions> options;
    HRESULT result = environment10->CreateCoreWebView2ControllerOptions(&options);
    if (FAILED(result) || !options) return FAILED(result) ? result : E_FAIL;
    const AgentBrowserWindowsProfile profile = agent_browser_windows_profile(key);
    result = options->put_ProfileName(acecode::utf8_to_wide(profile.name).c_str());
    if (FAILED(result)) return result;
    result = options->put_IsInPrivateModeEnabled(profile.in_private ? TRUE : FALSE);
    if (FAILED(result)) return result;
    return environment10->CreateCoreWebView2ControllerWithOptions(
        browser_widget, options.Get(), handler);
}

Microsoft::WRL::ComPtr<ICoreWebView2Profile> profile_of(
    const Microsoft::WRL::ComPtr<ICoreWebView2>& webview) {
    Microsoft::WRL::ComPtr<ICoreWebView2_13> webview13;
    Microsoft::WRL::ComPtr<ICoreWebView2Profile> profile;
    if (webview && SUCCEEDED(webview.As(&webview13)) && webview13 &&
        SUCCEEDED(webview13->get_Profile(&profile))) {
        return profile;
    }
    return nullptr;
}

void clear_profile(const Microsoft::WRL::ComPtr<ICoreWebView2Profile>& profile,
                          const std::shared_ptr<ProfileLease>& lease,
                          const StoreDone& done) {
    Microsoft::WRL::ComPtr<ICoreWebView2Profile2> profile2;
    if (!profile || FAILED(profile.As(&profile2)) || !profile2) {
        done(false, "WebView2 runtime cannot clear browsing data");
        return;
    }
    auto completed = Microsoft::WRL::Callback<
        ICoreWebView2ClearBrowsingDataCompletedHandler>(
        [lease, done](HRESULT result) -> HRESULT {
            done(SUCCEEDED(result),
                 SUCCEEDED(result) ? std::string()
                                   : "failed to clear browsing data (" +
                                         hresult_text(result) + ")");
            return S_OK;
        });
    const HRESULT result = profile2->ClearBrowsingDataAll(completed.Get());
    if (FAILED(result)) {
        done(false, "failed to clear browsing data (" + hresult_text(result) + ")");
    }
}

Microsoft::WRL::ComPtr<ICoreWebView2CookieManager> cookie_manager_of(
    const Microsoft::WRL::ComPtr<ICoreWebView2Profile>& profile) {
    Microsoft::WRL::ComPtr<ICoreWebView2Profile5> profile5;
    Microsoft::WRL::ComPtr<ICoreWebView2CookieManager> manager;
    if (profile && SUCCEEDED(profile.As(&profile5)) && profile5 &&
        SUCCEEDED(profile5->get_CookieManager(&manager))) {
        return manager;
    }
    return nullptr;
}


void copy_profile_cookies(
    const Microsoft::WRL::ComPtr<ICoreWebView2CookieManager>& source,
    const Microsoft::WRL::ComPtr<ICoreWebView2CookieManager>& target,
    const std::shared_ptr<ProfileLease>& lease,
    const std::string& parent_session_id,
    const std::function<void()>& done) {
    auto completed = Microsoft::WRL::Callback<
        ICoreWebView2GetCookiesCompletedHandler>(
        [target, done, lease, parent_session_id](
            HRESULT result, ICoreWebView2CookieList* list) -> HRESULT {
            UINT32 count = 0;
            std::size_t copied = 0;
            if (SUCCEEDED(result) && list && SUCCEEDED(list->get_Count(&count))) {
                for (UINT32 index = 0; index < count; ++index) {
                    Microsoft::WRL::ComPtr<ICoreWebView2Cookie> cookie;
                    Microsoft::WRL::ComPtr<ICoreWebView2Cookie> copy;
                    if (SUCCEEDED(list->GetValueAtIndex(index, &cookie)) && cookie &&
                        SUCCEEDED(target->CopyCookie(cookie.Get(), &copy)) && copy &&
                        SUCCEEDED(target->AddOrUpdateCookie(copy.Get()))) {
                        ++copied;
                    }
                }
            }
            LOG_INFO("[agent-browser] seeded forked cookie store from " +
                     parent_session_id + ": copied=" + std::to_string(copied) +
                     " of " + std::to_string(count));
            done();
            return S_OK;
        });
    if (FAILED(source->GetCookies(L"", completed.Get()))) {
        LOG_WARN("[agent-browser] failed to read parent cookie store " +
                 parent_session_id);
        done();
    }
}

} // namespace acecode::desktop::agent_browser_windows
#endif
