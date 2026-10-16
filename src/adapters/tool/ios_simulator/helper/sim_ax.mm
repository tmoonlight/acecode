#import "sim_ax.h"
#import "sim_core.h"
#import "private_api.h"

#include <cmath>
#include <unistd.h>
#include <mutex>

using acecode::ios_simulator::helper::json;

// Answers AXPTranslator's synchronous bridge callbacks by forwarding each
// translator request to the simulator the token belongs to.
@interface ACEAXTokenDelegate : NSObject {
    NSMutableDictionary<NSString*, id>* _devices;
    dispatch_queue_t _completion;
}
- (void)bindToken:(NSString*)token device:(id)device;
- (void)unbindToken:(NSString*)token;
@end

@implementation ACEAXTokenDelegate

- (instancetype)init {
    if ((self = [super init])) {
        _devices = [NSMutableDictionary dictionary];
        _completion = dispatch_queue_create("dev.acecode.ios-sim.ax.completion", DISPATCH_QUEUE_SERIAL);
    }
    return self;
}

- (void)bindToken:(NSString*)token device:(id)device {
    @synchronized(self) { _devices[token] = device; }
}

- (void)unbindToken:(NSString*)token {
    @synchronized(self) { [_devices removeObjectForKey:token]; }
}

- (id)deviceForToken:(NSString*)token {
    @synchronized(self) { return _devices[token]; }
}

- (ACEAXPTranslationCallback)accessibilityTranslationDelegateBridgeCallbackWithToken:(NSString*)token {
    id device = [self deviceForToken:token];
    dispatch_queue_t completion = _completion;
    return ^id(id request) {
        if (!device || !request) return nil;
        dispatch_semaphore_t answered = dispatch_semaphore_create(0);
        __block id response = nil;
        @try {
            [(id<ACESimDevice>)device sendAccessibilityRequestAsync:request completionQueue:completion
                completionHandler:^(id value) {
                    response = value;
                    dispatch_semaphore_signal(answered);
                }];
        } @catch (NSException*) {
            return nil;
        }
        const long waited = dispatch_semaphore_wait(answered, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));
        if (getenv("ACECODE_IOS_SIM_AX_DEBUG"))
            acecode::ios_simulator::helper::emit_log("debug", std::string("ax request ") +
                acecode::ios_simulator::helper::utf8([request description]).substr(0, 300) +
                (waited ? " TIMEOUT" : " -> ") +
                acecode::ios_simulator::helper::utf8([response description]).substr(0, 300));
        return response;
    };
}

- (CGRect)accessibilityTranslationConvertPlatformFrameToSystem:(CGRect)rect withToken:(NSString*)token {
    (void)token;
    return rect;
}

- (id)accessibilityTranslationRootParentWithToken:(NSString*)token {
    (void)token;
    return nil;
}

@end

namespace acecode::ios_simulator::helper {
namespace {

ACEAXTokenDelegate* shared_delegate() {
    static ACEAXTokenDelegate* delegate = [[ACEAXTokenDelegate alloc] init];
    return delegate;
}

std::string text_of(id value) {
    if (!value) return {};
    if ([value isKindOfClass:[NSString class]]) return utf8(value);
    if ([value isKindOfClass:[NSAttributedString class]]) return utf8([(NSAttributedString*)value string]);
    if ([value isKindOfClass:[NSNumber class]]) return utf8([(NSNumber*)value stringValue]);
    return {};
}

void set_text(json& node, const char* key, const std::string& value) {
    if (!value.empty()) node[key] = value;
}

double rounded(double value) { return std::round(value * 10.0) / 10.0; }

struct Walk {
    NSString* token;
    int max_depth;
    int max_nodes;
    int nodes = 0;
    bool truncated = false;
};

std::string role_name(NSString* value) {
    std::string role = utf8(value);
    if (role.rfind("AX", 0) == 0) role = role.substr(2);
    return role;
}

json frame_json(NSRect frame) {
    return {{"x", rounded(frame.origin.x)}, {"y", rounded(frame.origin.y)},
            {"width", rounded(frame.size.width)}, {"height", rounded(frame.size.height)}};
}

bool flag(id value) {
    return [value respondsToSelector:@selector(boolValue)] && [value boolValue];
}

// Reads every attribute of one element. Prefers the private batch read (one
// translator request per element); falls back to the NSAccessibility getters.
NSArray* read_element(id element, json& node, bool want_children) {
    id<ACEAXPPlatformElement> e = (id<ACEAXPPlatformElement>)element;
    if ([element respondsToSelector:@selector(accessibilityMultipleAttributes:)]) {
        NSMutableArray* names = [@[@"AXFrame", @"AXRole", @"AXDescription", @"AXValue", @"AXIdentifier", @"AXTitle",
                                   @"AXHelp", @"AXEnabled", @"AXPlaceholderValue", @"AXFocused", @"AXSelected"]
            mutableCopy];
        if (want_children) [names addObject:@"AXChildren"];
        NSDictionary* values = [e accessibilityMultipleAttributes:names];
        if ([values isKindOfClass:[NSDictionary class]] && values.count) {
            auto string_of = ^std::string(NSString* key) {
                id value = values[key];
                return value == [NSNull null] ? std::string() : text_of(value);
            };
            set_text(node, "role", role_name(values[@"AXRole"] == [NSNull null] ? nil : values[@"AXRole"]));
            set_text(node, "label", string_of(@"AXDescription"));
            set_text(node, "value", string_of(@"AXValue"));
            set_text(node, "identifier", string_of(@"AXIdentifier"));
            set_text(node, "title", string_of(@"AXTitle"));
            set_text(node, "help", string_of(@"AXHelp"));
            set_text(node, "placeholder", string_of(@"AXPlaceholderValue"));
            id frame = values[@"AXFrame"];
            node["frame"] = frame_json([frame isKindOfClass:[NSValue class]] ? [frame rectValue] : NSZeroRect);
            id enabled = values[@"AXEnabled"];
            if (enabled && enabled != [NSNull null] && !flag(enabled)) node["enabled"] = false;
            if (flag(values[@"AXSelected"])) node["selected"] = true;
            if (flag(values[@"AXFocused"])) node["focused"] = true;
            id children = values[@"AXChildren"];
            return [children isKindOfClass:[NSArray class]] ? children : @[];
        }
    }
    set_text(node, "role", role_name(e.accessibilityRole));
    set_text(node, "label", utf8(e.accessibilityLabel));
    set_text(node, "value", text_of(e.accessibilityValue));
    set_text(node, "identifier", utf8(e.accessibilityIdentifier));
    set_text(node, "title", utf8(e.accessibilityTitle));
    set_text(node, "help", utf8(e.accessibilityHelp));
    set_text(node, "placeholder", utf8(e.accessibilityPlaceholderValue));
    node["frame"] = frame_json(e.accessibilityFrame);
    if ([element respondsToSelector:@selector(isAccessibilityEnabled)] && !e.isAccessibilityEnabled)
        node["enabled"] = false;
    if ([element respondsToSelector:@selector(isAccessibilitySelected)] && e.isAccessibilitySelected)
        node["selected"] = true;
    if ([element respondsToSelector:@selector(isAccessibilityFocused)] && e.isAccessibilityFocused)
        node["focused"] = true;
    return want_children ? (e.accessibilityChildren ?: @[]) : @[];
}

json serialize(id element, Walk& walk, int depth, bool with_children) {
    json node = json::object();
    if (!element) return node;
    ++walk.nodes;
    @try {
        id translation = [(id<ACEAXPPlatformElement>)element translation];
        if ([translation respondsToSelector:@selector(setBridgeDelegateToken:)])
            [(id<ACEAXPTranslationObject>)translation setBridgeDelegateToken:walk.token];
        NSArray* children = read_element(element, node, with_children);
        if (with_children && children.count) {
            if (depth >= walk.max_depth) {
                walk.truncated = true;
            } else {
                json list = json::array();
                for (id child in children) {
                    if (walk.nodes >= walk.max_nodes) { walk.truncated = true; break; }
                    list.push_back(serialize(child, walk, depth + 1, true));
                }
                node["children"] = std::move(list);
            }
        }
    } @catch (NSException* exception) {
        node["error"] = utf8(exception.reason);
    }
    return node;
}

} // namespace

json describe_ui(id device, const std::string& udid, const UiQuery& query) {
    std::string load_error;
    if (!load_accessibility_framework(load_error))
        throw SimError("AX_UNAVAILABLE", "AccessibilityPlatformTranslation is unavailable: " + load_error);
    if (!device_booted(device)) throw SimError("DEVICE_NOT_BOOTED", "The simulator is not booted.");
    Class factory = NSClassFromString(@"AXPTranslator");
    __block id translator = nil;
    // Platform elements route their attribute reads through the process-wide
    // sharedInstance, so the token delegate must be installed there.
    guarded("AX_UNAVAILABLE", ^{
        if ([factory respondsToSelector:@selector(sharedInstance)])
            translator = [(id<ACEAXPTranslatorFactory>)factory sharedInstance];
        if (!translator && [factory respondsToSelector:@selector(sharediOSInstance)])
            translator = [(id<ACEAXPTranslatorFactory>)factory sharediOSInstance];
    });
    if (!translator) throw SimError("AX_UNAVAILABLE", "AXPTranslator has no shared instance.");
    ACEAXTokenDelegate* delegate = shared_delegate();
    NSString* token = [NSString stringWithFormat:@"acecode-%@-%@", ns(udid), [NSUUID UUID].UUIDString];
    [delegate bindToken:token device:device];
    json result = json::object();
    @try {
        id<ACEAXPTranslator> ax = (id<ACEAXPTranslator>)translator;
        ax.bridgeTokenDelegate = delegate;
        ax.supportsDelegateTokens = YES;
        ax.accessibilityEnabled = YES;
        id translation = nil;
        // The guest accessibility server starts on the first request; the
        // first answers after a boot or app launch are often empty.
        for (int attempt = 0; attempt < 3 && !translation; ++attempt) {
            if (attempt) usleep(700 * 1000);
            translation = query.mode == "point"
                ? [ax objectAtPoint:CGPointMake(query.x, query.y) displayId:0 bridgeDelegateToken:token]
                : [ax frontmostApplicationWithDisplayId:0 bridgeDelegateToken:token];
        }
        if (!translation) {
            [delegate unbindToken:token];
            throw SimError("AX_EMPTY", query.mode == "point"
                ? "No accessibility element at that point."
                : "No frontmost application answered the accessibility request.");
        }
        [(id<ACEAXPTranslationObject>)translation setBridgeDelegateToken:token];
        id element = [ax macPlatformElementFromTranslation:translation];
        if (!element) {
            [delegate unbindToken:token];
            throw SimError("AX_EMPTY", "The accessibility translation produced no element.");
        }
        Walk walk{token, std::max(1, query.max_depth), std::max(1, query.max_nodes)};
        result["root"] = serialize(element, walk, 0, query.mode != "point");
        result["pid"] = [(id<ACEAXPTranslationObject>)translation pid];
        result["node_count"] = walk.nodes;
        result["truncated"] = walk.truncated;
        result["coordinate_space"] = "device_points";
    } @catch (NSException* exception) {
        [delegate unbindToken:token];
        throw SimError("AX_FAILED", utf8(exception.name) + ": " + utf8(exception.reason));
    }
    [delegate unbindToken:token];
    return result;
}

} // namespace acecode::ios_simulator::helper
