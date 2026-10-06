#pragma once

// Cloudflare 快速隧道(trycloudflare.com)管理:为本机 LINE 回调端口申请一个临时公网 HTTPS 地址。
//
//   cloudflared tunnel --no-autoupdate --metrics 127.0.0.1:<空闲端口> --url http://127.0.0.1:<回调端口>
//
// 公网主机名从 cloudflared 本机指标服务读取:GET /quicktunnel → {"hostname":"xxx.trycloudflare.com"}
// (分配前为空串);GET /ready 返回 200 表示已与 Cloudflare 边缘建立连接。
// 只探测已安装的 cloudflared(PATH 与 Windows 常见安装位置),绝不自动下载。
// 隧道只指向 LINE 专用的回调端口,绝不指向 ACECode 主 Web 端口。
//
// 线程:CloudflareTunnel 不是线程安全的,只由传输层的连接线程使用;真实子进程的输出由
// 内部 JoiningThread 持续读空,避免管道写满卡住子进程。

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace acecode::im::line {

// 已启动的 cloudflared 子进程。测试注入假实现。
class TunnelProcess {
public:
    virtual ~TunnelProcess() = default;
    virtual bool alive() = 0;  // 进程是否还在运行
    virtual void stop() = 0;   // 结束进程并回收;阻塞,可重复调用
};

using TunnelLauncher =
    std::function<std::unique_ptr<TunnelProcess>(const std::vector<std::string>& argv, std::string* error)>;

struct TunnelOptions {
    std::string cloudflared_path;          // 用户指定的路径;为空时自动查找
    std::function<std::string()> locate;   // 测试用:替换自动查找;返回空串表示没装
    TunnelLauncher launch;                 // 测试用:替换真实子进程
    std::uint16_t metrics_port = 0;        // 0 = 每次启动挑一个本机空闲端口
    std::string protocol;                  // 空 = cloudflared 默认(quic);UDP 被封的网络可设 "http2"
    std::string config_path;               // 可选:指定一个空配置文件,避免读到 ~/.cloudflared/config.yml
    std::string log_file;                  // 可选:cloudflared 自己的日志文件(排查隧道问题用)
    std::string url_scheme = "https";      // 测试用 http
    std::chrono::milliseconds ready_timeout{std::chrono::seconds(45)};
    std::chrono::milliseconds poll_interval{std::chrono::milliseconds(500)};
};

// 用户安装 cloudflared 的提示(状态文字里用)。
inline constexpr const char* kCloudflaredInstallHint =
    "未找到 cloudflared:请运行 winget install --id Cloudflare.cloudflared 安装后重新连接,或填写自己的公网 HTTPS 地址";

// 查找顺序:PATH 上的 cloudflared,然后 Windows 常见安装位置。找不到返回空串。
std::string locate_cloudflared();
// 纯函数:Windows 常见安装位置(按优先级);env 取环境变量(UTF-8),没有时返回空串。
std::vector<std::string> cloudflared_install_candidates(const std::function<std::string(const char*)>& env);

std::vector<std::string> cloudflared_argv(const std::string& executable, std::uint16_t target_port,
                                          std::uint16_t metrics_port, const TunnelOptions& options);

// 解析 /quicktunnel 的应答;主机名为空或格式不对时返回 nullopt。
std::optional<std::string> parse_quicktunnel_hostname(const std::string& body);

// 在 127.0.0.1 上找一个当前空闲的 TCP 端口;失败返回 0。
std::uint16_t pick_free_loopback_port();

enum class TunnelFailure { None, Missing, Launch, Exited, Timeout, Cancelled };

class CloudflareTunnel {
public:
    explicit CloudflareTunnel(TunnelOptions options);
    ~CloudflareTunnel();
    CloudflareTunnel(const CloudflareTunnel&) = delete;
    CloudflareTunnel& operator=(const CloudflareTunnel&) = delete;

    // 启动 cloudflared 指向 http://127.0.0.1:<target_port>,等到拿到主机名且 /ready 为 200。
    // 成功返回公网地址(如 https://abc.trycloudflare.com);失败返回空串并给出原因。
    std::string start(std::uint16_t target_port, const std::function<bool()>& cancel, TunnelFailure* failure,
                      std::string* error);
    bool running() const { return static_cast<bool>(process_); }
    bool alive();
    // 指标服务 /ready 是否为 200(进程还在但与边缘断开时为 false)。
    bool ready();
    void stop();
    const std::string& public_url() const { return public_url_; }

private:
    std::string executable();

    TunnelOptions options_;
    std::unique_ptr<TunnelProcess> process_;
    std::uint16_t metrics_port_ = 0;
    std::string public_url_;
};

} // namespace acecode::im::line
