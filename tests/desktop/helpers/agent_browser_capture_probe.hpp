#pragma once

#include "tool/agent_browser/cdp_client.hpp"

#ifdef _WIN32
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#else
#import <AppKit/AppKit.h>
#endif

#include <array>
#include <string>

namespace acecode::desktop::capture_probe {

inline bool green_bottom_pixel(const std::vector<unsigned char>& bytes) {
#ifdef _WIN32
    struct ComScope {
        HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ~ComScope() { if (SUCCEEDED(result)) CoUninitialize(); }
    } com_scope;
    if (FAILED(com_scope.result) && com_scope.result != RPC_E_CHANGED_MODE) return false;
    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    Microsoft::WRL::ComPtr<IWICStream> stream;
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&factory))) || FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()),
            static_cast<DWORD>(bytes.size()))) ||
        FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr,
            WICDecodeMetadataCacheOnLoad, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) ||
        FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
            WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return false;
    UINT width = 0, height = 0;
    if (FAILED(converter->GetSize(&width, &height)) || width < 2 || height < 20) return false;
    WICRect rect{static_cast<INT>(width / 2), static_cast<INT>(height - 10), 1, 1};
    std::array<BYTE, 4> pixel{};
    return SUCCEEDED(converter->CopyPixels(&rect, 4, 4, pixel.data())) &&
        pixel[0] < 10 && pixel[1] > 245 && pixel[2] < 10 && pixel[3] > 245;
#else
    NSData* data = [NSData dataWithBytes:bytes.data() length:bytes.size()];
    NSBitmapImageRep* image = [NSBitmapImageRep imageRepWithData:data];
    if (!image || image.pixelsWide < 2 || image.pixelsHigh < 20) return false;
    NSColor* color = [[image colorAtX:image.pixelsWide / 2 y:image.pixelsHigh - 10]
        colorUsingColorSpace:[NSColorSpace deviceRGBColorSpace]];
    return color && color.redComponent < 0.04 && color.greenComponent > 0.96 &&
        color.blueComponent < 0.04 && color.alphaComponent > 0.96;
#endif
}

inline bool run(agent_browser::AgentBrowserCdpClient& client, std::string& error) {
    using namespace std::chrono_literals;
    const auto evaluate = [&client, &error](const std::string& expression) {
        return client.command("Runtime.evaluate", {{"expression", expression},
            {"awaitPromise", true}, {"returnByValue", true}}, 10s, nullptr, error)
            .value("result", nlohmann::json::object()).value("value", nlohmann::json());
    };
    const auto fixture = evaluate(R"JS((async () => {
        document.documentElement.style.cssText = 'margin:0;padding:0;border:0;overflow:auto';
        document.body.style.cssText = 'margin:0;padding:0;border:0';
        const height = Math.max(2400, innerHeight * 3);
        document.body.innerHTML = '<div style="height:' + (height - 100) +
            'px;background:rgb(255,0,0)"></div><div style="height:100px;background:rgb(0,255,0)"></div>';
        scrollTo(0, 100);
        await new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)));
        return {height, scroll:scrollY, viewport:innerHeight, width:innerWidth};
    })())JS");
    if (!error.empty() || !fixture.is_object()) return false;
    const auto viewport = client.command("Page.captureScreenshot", {{"format", "png"}},
        20s, nullptr, error);
    const auto viewport_bytes = agent_browser::decode_agent_browser_base64(viewport.value("data", ""));
    const auto viewport_size = viewport_bytes
        ? agent_browser::agent_browser_png_dimensions(*viewport_bytes) : std::nullopt;
    if (!error.empty() || !viewport_size) { error = "viewport capture failed"; return false; }
    const auto full = client.command("Page.captureScreenshot", {{"fullPage", true}},
        20s, nullptr, error);
    const auto bytes = agent_browser::decode_agent_browser_base64(full.value("data", ""));
    const auto size = bytes ? agent_browser::agent_browser_png_dimensions(*bytes) : std::nullopt;
    if (!error.empty() || !size || full.value("fullPage", false) != true ||
        size->second != fixture["height"].get<unsigned>() ||
        size->second <= fixture["viewport"].get<unsigned>() || !green_bottom_pixel(*bytes)) {
        if (error.empty()) error = "full-page PNG did not contain the beyond-viewport green pixels";
        return false;
    }
    const auto after = evaluate("({scroll:scrollY, viewport:innerHeight, width:innerWidth})");
    if (!error.empty() || after["scroll"] != fixture["scroll"] ||
        after["viewport"] != fixture["viewport"] || after["width"] != fixture["width"]) {
        error = "full-page capture changed viewport or scroll"; return false;
    }
    client.command("Page.captureScreenshot", {{"fullPage", true}, {"documentId", "stale-document"}},
        20s, nullptr, error);
    if (error != "capture_document_changed") { error = "stale document capture was not rejected"; return false; }
    error.clear();
    evaluate("document.body.firstElementChild.style.height = '40000px'");
    if (!error.empty()) return false;
    client.command("Page.captureScreenshot", {{"fullPage", true}}, 20s, nullptr, error);
    if (error != "capture_resource_limit") { error = "oversized capture was not rejected"; return false; }
    error.clear();
    evaluate("document.body.firstElementChild.style.height = " +
        std::to_string(fixture["height"].get<int>() - 100));
    if (!error.empty()) return false;
    const auto retry = client.command("Page.captureScreenshot", {{"fullPage", true},
        {"documentId", full.at("documentId")}}, 20s, nullptr, error);
    return error.empty() && retry.value("fullPage", false);
}

}
