#include "agent_browser_capture_mac.hpp"

#ifdef __APPLE__
#include "agent_browser_macos_helpers.hpp"
#include "utils/uuid.hpp"

#import <AppKit/AppKit.h>
#import <WebKit/WebKit.h>

namespace acecode::desktop::browser_capture {
using nlohmann::json;

void capture_webkit(std::shared_ptr<WKWebView> view, std::string page_id,
                    const json& params, Evaluate evaluate, Current current, Complete complete,
                    Validate validate) {
    bool full_page = false;
    const auto request_error = request_mode(params, full_page);
    if (!request_error.empty()) {
        complete({{"ok", false}, {"page_id", page_id},
            {"error", request_error}, {"recoverable", true}});
        return;
    }
    if (full_page) {
        const Measure measure = [evaluate, current](Measured done) {
            if (!current()) { done({}, "capture_document_changed"); return; }
            const auto expression = std::string(R"JS((() => {
                const key = '__acecodeFullPageCaptureDocument';
                if (!globalThis[key] || globalThis[key].document !== document) {
                    globalThis[key] = { document, id: )JS") +
                        json(acecode::generate_uuid()).dump() + R"JS( };
                }
                const root = document.documentElement;
                const body = document.body;
                return { document: globalThis[key].id,
                    width: Math.max(innerWidth, root?.scrollWidth || 0,
                        root?.offsetWidth || 0, body?.scrollWidth || 0, body?.offsetWidth || 0),
                    height: Math.max(innerHeight, root?.scrollHeight || 0,
                        root?.offsetHeight || 0, body?.scrollHeight || 0, body?.offsetHeight || 0) };
            })())JS";
            evaluate(expression, [done](bool ok, json value, std::string error) {
                if (!ok) { done({}, "capture_measure_failed: " + error); return; }
                if (!value.is_object() || !value.contains("width") ||
                    !value["width"].is_number() || !value.contains("height") ||
                    !value["height"].is_number() || !value.contains("document") ||
                    !value["document"].is_string()) {
                    done({}, "capture_invalid_bounds"); return;
                }
                done({0, 0, value["width"].get<double>(), value["height"].get<double>(),
                    value["document"].get<std::string>()}, {});
            });
        };
        const Capture capture = [view, current](const Bounds& bounds, Captured done) {
            if (!current()) { done({}, "capture_document_changed"); return; }
            WKPDFConfiguration* configuration = [[[WKPDFConfiguration alloc] init] autorelease];
            const auto width = static_cast<std::size_t>(std::ceil(bounds.width));
            const auto height = static_cast<std::size_t>(std::ceil(bounds.height));
            configuration.rect = CGRectMake(bounds.x, bounds.y, width, height);
            [view.get() createPDFWithConfiguration:configuration
                completionHandler:^(NSData* data, NSError* error) {
                if (!current()) { done({}, "capture_document_changed"); return; }
                if (error || !data) { done({}, "capture_native_failed"); return; }
                if (data.length > kMaxPngBytes) { done({}, "capture_resource_limit"); return; }
                const auto provider = std::unique_ptr<CGDataProvider, decltype(&CGDataProviderRelease)>(
                    CGDataProviderCreateWithCFData((CFDataRef)data), CGDataProviderRelease);
                const auto pdf = std::unique_ptr<CGPDFDocument, decltype(&CGPDFDocumentRelease)>(
                    provider ? CGPDFDocumentCreateWithProvider(provider.get()) : nullptr,
                    CGPDFDocumentRelease);
                if (!pdf || CGPDFDocumentGetNumberOfPages(pdf.get()) != 1) {
                    done({}, "capture_incomplete_image"); return;
                }
                const auto pdf_page = CGPDFDocumentGetPage(pdf.get(), 1);
                const CGRect box = CGPDFPageGetBoxRect(pdf_page, kCGPDFMediaBox);
                if (std::abs(box.size.width - width) > 0.01 ||
                    std::abs(box.size.height - height) > 0.01) {
                    done({}, "capture_incomplete_image"); return;
                }
                const auto color = std::unique_ptr<CGColorSpace, decltype(&CGColorSpaceRelease)>(
                    CGColorSpaceCreateDeviceRGB(), CGColorSpaceRelease);
                if (!color) { done({}, "capture_resource_limit"); return; }
                const auto bitmap = std::unique_ptr<CGContext, decltype(&CGContextRelease)>(
                    CGBitmapContextCreate(nullptr, width, height, 8, width * 4,
                        color.get(), kCGImageAlphaPremultipliedLast), CGContextRelease);
                if (!bitmap) { done({}, "capture_resource_limit"); return; }
                CGContextSetRGBFillColor(bitmap.get(), 1, 1, 1, 1);
                CGContextFillRect(bitmap.get(), CGRectMake(0, 0, width, height));
                CGContextConcatCTM(bitmap.get(), CGPDFPageGetDrawingTransform(
                    pdf_page, kCGPDFMediaBox, CGRectMake(0, 0, width, height), 0, false));
                CGContextDrawPDFPage(bitmap.get(), pdf_page);
                const auto image = std::unique_ptr<CGImage, decltype(&CGImageRelease)>(
                    CGBitmapContextCreateImage(bitmap.get()), CGImageRelease);
                if (!image) { done({}, "capture_native_failed"); return; }
                NSBitmapImageRep* representation = [[[NSBitmapImageRep alloc]
                    initWithCGImage:image.get()] autorelease];
                NSData* png = [representation representationUsingType:NSBitmapImageFileTypePNG
                    properties:@{}];
                if (!png || png.length > kMaxPngBytes) { done({}, "capture_resource_limit"); return; }
                try {
                    done(std::string(static_cast<const char*>(png.bytes), png.length), {});
                } catch (const std::bad_alloc&) {
                    done({}, "capture_resource_limit");
                }
            }];
        };
        std::make_shared<FullPageCapture>(page_id, params.value("documentId", ""),
            validated_measure(measure, current, std::move(validate)), capture, current, complete)->start();
        return;
    }
    WKSnapshotConfiguration* configuration = [[[WKSnapshotConfiguration alloc] init] autorelease];
    configuration.rect = [view.get() bounds];
    [view.get() takeSnapshotWithConfiguration:configuration
        completionHandler:^(NSImage* image, NSError* error) {
        if (error || !image) {
            complete({{"ok", false}, {"page_id", page_id},
                {"error", error ? agent_browser_macos::utf8_string([error localizedDescription])
                                : "WKWebView snapshot failed"}});
            return;
        }
        CGImageRef cg_image = [image CGImageForProposedRect:nil context:nil hints:nil];
        if (!cg_image) {
            complete({{"ok", false}, {"page_id", page_id},
                {"error", "WKWebView snapshot returned no image"}});
            return;
        }
        NSBitmapImageRep* representation = [[[NSBitmapImageRep alloc]
            initWithCGImage:cg_image] autorelease];
        NSData* data = [representation representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
        if (!data || [data length] == 0) {
            complete({{"ok", false}, {"page_id", page_id},
                {"error", "failed to encode WKWebView snapshot"}});
            return;
        }
        const std::string bytes(static_cast<const char*>([data bytes]), [data length]);
        complete({{"ok", true}, {"page_id", page_id},
            {"result", {{"data", acecode::base64_encode(bytes)}, {"captureBeyondViewport", false}}}});
    }];
}

}
#endif
