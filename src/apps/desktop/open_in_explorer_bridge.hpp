#pragma once

#include "utils/joining_thread.hpp"
#include "utils/lifetime_token.hpp"

namespace acecode::desktop {

class WebHost;

// aceDesktop_openInExplorer([path]) → {ok, error?}。Windows 上打开文件夹要等
// 资源管理器窗口出现(0.3–0.6 秒),所以在后台线程执行、完成后异步回复,
// WebView UI 线程不被占用。必须在 host 之后构造、先于 host 析构。
class OpenInExplorerBridge {
public:
    // Not copyable or movable: the bridge binding holds a lifetime ref to *this.
    explicit OpenInExplorerBridge(WebHost& host);
    ~OpenInExplorerBridge();

private:
    ReapingThreadSet workers_;
    LifetimeToken lifetime_;  // Last member: revoked before workers_ is joined.
};

} // namespace acecode::desktop
