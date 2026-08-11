#include "http.hpp"

#include "../network/proxy_resolver.hpp"

#include <cpr/cpr.h>
#include <atomic>
#include <fstream>

namespace acecode::upgrade {

HttpTextResult fetch_text(const std::string& url,
                          int timeout_ms,
                          const HttpCancelCheck& cancel_check) {
    HttpTextResult out;
    std::atomic<bool> cancel_observed{false};
    auto progress_cb = cpr::ProgressCallback{
        [&](cpr::cpr_off_t,
            cpr::cpr_off_t,
            cpr::cpr_off_t,
            cpr::cpr_off_t,
            intptr_t) -> bool {
            if (cancel_check && cancel_check()) {
                cancel_observed.store(true);
                return false;
            }
            return true;
        }
    };
    auto proxy_opts = network::proxy_options_for(url);
    cpr::Response r = cpr::Get(
        cpr::Url{url},
        cpr::Header{{"Accept", "application/json"}, {"User-Agent", "acecode-updater"}},
        network::build_ssl_options(proxy_opts),
        proxy_opts.proxies,
        proxy_opts.auth,
        cpr::Timeout{timeout_ms},
        progress_cb);

    out.status_code = r.status_code;
    out.body = std::move(r.text);
    out.cancelled = cancel_observed.load();
    if (!out.cancelled && r.error.code != cpr::ErrorCode::OK) {
        out.error = r.error.message;
    }
    return out;
}

DownloadResult download_to_file(const std::string& url,
                                const std::filesystem::path& output_path,
                                int timeout_ms) {
    return download_to_file(url, output_path, timeout_ms, DownloadProgressCallback{});
}

DownloadResult download_to_file(const std::string& url,
                                const std::filesystem::path& output_path,
                                int timeout_ms,
                                const DownloadProgressCallback& progress_cb,
                                const HttpCancelCheck& cancel_check) {
    DownloadResult out;
    if (cancel_check && cancel_check()) {
        out.cancelled = true;
        return out;
    }
    std::ofstream ofs(output_path, std::ios::binary);
    if (!ofs) {
        out.error = "failed to open output file: " + output_path.string();
        return out;
    }

    std::atomic<bool> cancel_observed{false};
    auto cancelled = [&]() {
        if (!cancel_check || !cancel_check()) return false;
        cancel_observed.store(true);
        return true;
    };
    auto write_cb = cpr::WriteCallback{
        [&](const std::string_view data, intptr_t) -> bool {
            if (cancelled()) return false;
            ofs.write(data.data(), static_cast<std::streamsize>(data.size()));
            if (!ofs) return false;
            out.bytes_written += static_cast<std::uintmax_t>(data.size());
            if (progress_cb) {
                progress_cb(DownloadProgress{out.bytes_written});
            }
            return true;
        }
    };
    auto progress_cb_cpr = cpr::ProgressCallback{
        [&](cpr::cpr_off_t,
            cpr::cpr_off_t,
            cpr::cpr_off_t,
            cpr::cpr_off_t,
            intptr_t) -> bool {
            return !cancelled();
        }
    };

    auto proxy_opts = network::proxy_options_for(url);
    cpr::Response r = cpr::Get(
        cpr::Url{url},
        cpr::Header{{"Accept", "application/zip, application/octet-stream, application/x-zip-compressed, */*"},
                    {"User-Agent", "acecode-updater"}},
        network::build_ssl_options(proxy_opts),
        proxy_opts.proxies,
        proxy_opts.auth,
        cpr::Timeout{timeout_ms},
        write_cb,
        progress_cb_cpr);

    out.status_code = r.status_code;
    out.cancelled = cancel_observed.load();
    if (!out.cancelled && r.error.code != cpr::ErrorCode::OK) {
        out.error = r.error.message;
    }
    return out;
}

} // namespace acecode::upgrade
