#import "sim_hid.h"
#import "sim_core.h"
#import "private_api.h"

#include <cstdlib>
#include <cstring>
#include <mach/mach_time.h>
#include <malloc/malloc.h>
#include <unistd.h>

static_assert(sizeof(ACEIndigoPayload) == 0x90, "Indigo payload layout");
static_assert(sizeof(ACEIndigoMessage) == 0xB0, "Indigo message layout");
static_assert(sizeof(ACEIndigoTouch) == 0x70, "Indigo touch layout");

namespace acecode::ios_simulator::helper {
namespace {

constexpr const char* kDigitizerService = "com.apple.coredevice.feature.remote.hid.digitizer";
constexpr const char* kFirstDTUHIDVersion = "1155.4";

bool version_at_least(const std::string& version, const char* minimum) {
    if (version.empty()) return false;
    return [ns(version) compare:@(minimum) options:NSNumericSearch] != NSOrderedAscending;
}

// ---------------------------------------------------------------------------
// Indigo (SimDeviceLegacyHIDClient)

class IndigoSession final : public HidSession {
public:
    IndigoSession(id device, double point_width, double point_height)
        : point_width_(point_width), point_height_(point_height) {
        button_fn_ = reinterpret_cast<ACEIndigoMessageForButtonFn>(simulator_kit_symbol("IndigoHIDMessageForButton"));
        keyboard_fn_ = reinterpret_cast<ACEIndigoMessageForKeyboardArbitraryFn>(
            simulator_kit_symbol("IndigoHIDMessageForKeyboardArbitrary"));
        arbitrary_fn_ = reinterpret_cast<ACEIndigoMessageForHIDArbitraryFn>(
            simulator_kit_symbol("IndigoHIDMessageForHIDArbitrary"));
        mouse_fn_ = reinterpret_cast<ACEIndigoMessageForMouseNSEventFn>(
            simulator_kit_symbol("IndigoHIDMessageForMouseNSEvent"));
        if (!button_fn_ || !keyboard_fn_ || !mouse_fn_)
            throw SimError("HID_UNAVAILABLE", "SimulatorKit Indigo message builders are unavailable: " +
                environment().load_error);
        Class client_class = NSClassFromString(@"SimulatorKit.SimDeviceLegacyHIDClient");
        if (!client_class) client_class = NSClassFromString(@"SimDeviceLegacyHIDClient");
        if (!client_class) throw SimError("HID_UNAVAILABLE", "SimDeviceLegacyHIDClient is unavailable.");
        __block id client = nil;
        __block NSError* failure = nil;
        guarded("HID_UNAVAILABLE", ^{
            client = [(id<ACESimDeviceLegacyHIDClient>)[client_class alloc] initWithDevice:device error:&failure];
        });
        if (!client) throw SimError("HID_UNAVAILABLE", "Could not create the HID client: " + describe_error(failure));
        client_ = client;
        queue_ = dispatch_queue_create("dev.acecode.ios-sim.indigo", DISPATCH_QUEUE_SERIAL);
        group_ = dispatch_group_create();
    }

    std::string transport() const override { return "indigo"; }

    void touch(Direction direction, double x, double y, unsigned edge) override {
        CGPoint point = ratio(x, y);
        ACEIndigoMessage* source = mouse_fn_(&point, nullptr, ACEIndigoMouseTarget, indigo_type(direction),
                                             CGSizeMake(1, 1), edge);
        if (!source) throw SimError("HID_FAILED", "Indigo touch builder returned no message.");
        source->payload.event.touch.xRatio = point.x;
        source->payload.event.touch.yRatio = point.y;
        // SimulatorKit only builds multi-touch envelopes; re-envelope the
        // digitizer contact as a two-payload single-touch message.
        const size_t size = sizeof(ACEIndigoMessage) + ACEIndigoPayloadStride;
        auto* bytes = static_cast<unsigned char*>(calloc(1, size));
        if (!bytes) { free(source); throw SimError("HID_FAILED", "Out of memory."); }
        auto* message = reinterpret_cast<ACEIndigoMessage*>(bytes);
        message->innerSize = static_cast<unsigned int>(ACEIndigoPayloadStride);
        message->eventType = ACEIndigoEventTypeTouch;
        message->payload.eventKind = 0x0000000B;
        message->payload.timestamp = mach_absolute_time();
        memcpy(bytes + 0x30, reinterpret_cast<unsigned char*>(source) + 0x30, sizeof(ACEIndigoTouch));
        free(source);
        memcpy(bytes + 0x20 + ACEIndigoPayloadStride, bytes + 0x20, ACEIndigoPayloadStride);
        auto* second = reinterpret_cast<ACEIndigoPayload*>(bytes + 0x20 + ACEIndigoPayloadStride);
        second->event.touch.field1 = 1;
        second->event.touch.field2 = 2;
        send(bytes);
    }

    void touch2(Direction direction, double x1, double y1, double x2, double y2) override {
        CGPoint first = ratio(x1, y1);
        CGPoint second = ratio(x2, y2);
        ACEIndigoMessage* message = mouse_fn_(&first, &second, ACEIndigoMouseTarget, indigo_type(direction),
                                              CGSizeMake(1, 1), 0);
        if (!message) throw SimError("HID_FAILED", "Indigo multi-touch builder returned no message.");
        auto* bytes = reinterpret_cast<unsigned char*>(message);
        message->payload.event.touch.xRatio = first.x;
        message->payload.event.touch.yRatio = first.y;
        auto* digitizer = reinterpret_cast<ACEIndigoPayload*>(bytes + ACEIndigoSecondPayloadWireOffset);
        digitizer->event.touch.xRatio = first.x;
        digitizer->event.touch.yRatio = first.y;
        auto* finger = reinterpret_cast<ACEIndigoPayload*>(
            bytes + ACEIndigoSecondPayloadWireOffset + ACEIndigoPayloadWireStride);
        finger->event.touch.xRatio = second.x;
        finger->event.touch.yRatio = second.y;
        send(bytes);
    }

    void button(const HidButton& button, Direction direction) override {
        ACEIndigoMessage* message = nullptr;
        if (button.indigo_source >= 0) {
            message = button_fn_(button.indigo_source, indigo_type(direction), ACEIndigoButtonTargetHardware);
        } else if (arbitrary_fn_) {
            message = arbitrary_fn_(ACEIndigoButtonTargetDigitizer, button.usage_page, button.usage,
                                    indigo_type(direction));
        }
        if (!message) throw SimError("HID_UNSUPPORTED", "This button is not available on the Indigo transport.");
        send(reinterpret_cast<unsigned char*>(message));
    }

    void key(std::uint32_t usage, Direction direction) override {
        ACEIndigoMessage* message = keyboard_fn_(static_cast<int>(usage), indigo_type(direction));
        if (!message) throw SimError("HID_FAILED", "Indigo keyboard builder returned no message.");
        send(reinterpret_cast<unsigned char*>(message));
    }

    ~IndigoSession() override {
        dispatch_group_wait(group_, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC));
    }

    void flush() override {
        if (dispatch_group_wait(group_, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) != 0)
            throw SimError("HID_TIMEOUT", "The simulator did not acknowledge HID input within 5 seconds.");
        __block std::string failure;
        std::shared_ptr<std::string> slot = first_failure_;
        dispatch_sync(queue_, ^{ failure.swap(*slot); });
        if (!failure.empty()) throw SimError("HID_FAILED", "HID delivery failed: " + failure);
    }

private:
    CGPoint ratio(double x, double y) const {
        return CGPointMake(point_width_ > 0 ? x / point_width_ : 0, point_height_ > 0 ? y / point_height_ : 0);
    }

    static unsigned int indigo_type(Direction direction) {
        return direction == Direction::Down ? ACEIndigoButtonEventTypeDown : ACEIndigoButtonEventTypeUp;
    }

    // Takes ownership of a malloc'd message; the client frees it.
    void send(unsigned char* bytes) {
        id client = client_;
        dispatch_queue_t queue = queue_;
        dispatch_group_t group = group_;
        std::shared_ptr<std::string> failure_slot = first_failure_;
        dispatch_group_enter(group);
        @try {
            [(id<ACESimDeviceLegacyHIDClient>)client sendWithMessage:bytes freeWhenDone:YES completionQueue:queue
                completion:^(NSError* error) {
                    if (error && failure_slot->empty()) *failure_slot = describe_error(error);
                    dispatch_group_leave(group);
                }];
        } @catch (NSException* exception) {
            // Ownership of the buffer is ambiguous after a raise; leak rather than double free.
            dispatch_group_leave(group);
            throw SimError("HID_FAILED", utf8(exception.reason));
        }
    }

    double point_width_;
    double point_height_;
    ACEIndigoMessageForButtonFn button_fn_ = nullptr;
    ACEIndigoMessageForKeyboardArbitraryFn keyboard_fn_ = nullptr;
    ACEIndigoMessageForHIDArbitraryFn arbitrary_fn_ = nullptr;
    ACEIndigoMessageForMouseNSEventFn mouse_fn_ = nullptr;
    id client_ = nil;
    dispatch_queue_t queue_ = nil;
    dispatch_group_t group_ = nil;
    // Written by completions on queue_, read on queue_ by flush().
    std::shared_ptr<std::string> first_failure_ = std::make_shared<std::string>();
};

// ---------------------------------------------------------------------------
// DTUHID (dtuhidd digitizer service over a simulator XPC endpoint)

class DtuhidSession final : public HidSession {
public:
    DtuhidSession(id device, double point_width, double point_height)
        : point_width_(point_width), point_height_(point_height) {
        std::string last_error;
        for (int attempt = 1; attempt <= 5; ++attempt) {
            bool unsupported = false;
            id connection = connect_guest_service(device, kDigitizerService, last_error, &unsupported);
            if (unsupported) throw SimError("HID_UNAVAILABLE", "This simulator runtime does not provide dtuhidd.");
            if (connection) {
                xpc_connection_t xpc = (xpc_connection_t)connection;
                if (confirm_liveness(xpc, last_error)) {
                    connection_ = xpc;
                    usleep(200 * 1000);
                    return;
                }
                xpc_connection_cancel(xpc);
            }
            if (attempt < 5) usleep(4 * 1000 * 1000);
        }
        throw SimError("HID_UNAVAILABLE", "dtuhidd did not respond: " + last_error);
    }

    ~DtuhidSession() override {
        if (connection_) xpc_connection_cancel(connection_);
    }

    std::string transport() const override { return "dtuhid"; }

    void touch(Direction direction, double x, double y, unsigned edge) override {
        xpc_object_t payload = xpc_dictionary_create(nullptr, nullptr, 0);
        xpc_dictionary_set_value(payload, "pointOne", point(x, y));
        xpc_dictionary_set_uint64(payload, "eventType", contact_phase(contact_active_, direction));
        xpc_dictionary_set_uint64(payload, "edge", edge);
        xpc_dictionary_set_uint64(payload, "target", 0);
        send("IndigoDigitizerEvent", payload);
    }

    void touch2(Direction direction, double x1, double y1, double x2, double y2) override {
        xpc_object_t payload = xpc_dictionary_create(nullptr, nullptr, 0);
        xpc_dictionary_set_value(payload, "pointOne", point(x1, y1));
        xpc_dictionary_set_value(payload, "pointTwo", point(x2, y2));
        xpc_dictionary_set_uint64(payload, "eventType", contact_phase(two_finger_active_, direction));
        xpc_dictionary_set_uint64(payload, "edge", 0);
        xpc_dictionary_set_uint64(payload, "target", 0);
        send("IndigoDigitizerEvent", payload);
    }

    void button(const HidButton& button, Direction direction) override {
        if (button.usage_page == 0)
            throw SimError("HID_UNSUPPORTED", "Apple Pay is a double side-button press on this transport; "
                "press side_button twice instead.");
        xpc_object_t payload = xpc_dictionary_create(nullptr, nullptr, 0);
        xpc_dictionary_set_uint64(payload, "usagePage", button.usage_page);
        xpc_dictionary_set_uint64(payload, "usageCode", button.usage);
        xpc_dictionary_set_uint64(payload, "state", direction == Direction::Down ? 1 : 2);
        send("IndigoButtonEvent", payload);
    }

    void key(std::uint32_t usage, Direction direction) override {
        xpc_object_t payload = xpc_dictionary_create(nullptr, nullptr, 0);
        xpc_dictionary_set_uint64(payload, "usageCode", usage);
        xpc_dictionary_set_uint64(payload, "state", direction == Direction::Down ? 1 : 2);
        send("IndigoKeyboardButtonEvent", payload);
    }

    void flush() override {
        if (sent_ > drained_) {
            usleep(80 * 1000);
            drained_ = sent_;
        }
    }

private:
    static xpc_object_t envelope(const char* type, xpc_object_t payload, bool barrier) {
        xpc_object_t message = xpc_dictionary_create(nullptr, nullptr, 0);
        xpc_dictionary_set_string(message, "messageType", type);
        xpc_dictionary_set_bool(message, "isBarrier", barrier);
        xpc_dictionary_set_string(message, "featureIdentifier", kDigitizerService);
        xpc_dictionary_set_value(message, "payload", payload);
        return message;
    }

    static bool confirm_liveness(xpc_connection_t connection, std::string& error) {
        xpc_object_t payload = xpc_dictionary_create(nullptr, nullptr, 0);
        xpc_dictionary_set_uint64(payload, "usageCode", 0);
        xpc_dictionary_set_uint64(payload, "state", 2);
        dispatch_semaphore_t answered = dispatch_semaphore_create(0);
        __block bool ok = false;
        __block std::string reply_error;
        xpc_connection_send_message_with_reply(connection, envelope("IndigoKeyboardButtonEvent", payload, true),
            dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^(xpc_object_t reply) {
                if (xpc_get_type(reply) == XPC_TYPE_ERROR) {
                    const char* description = xpc_dictionary_get_string(reply, XPC_ERROR_KEY_DESCRIPTION);
                    reply_error = description ? description : "XPC error";
                } else {
                    ok = true;
                }
                dispatch_semaphore_signal(answered);
            });
        if (dispatch_semaphore_wait(answered, dispatch_time(DISPATCH_TIME_NOW, 4 * NSEC_PER_SEC)) != 0) {
            error = "liveness probe timed out";
            return false;
        }
        if (!ok) error = reply_error;
        return ok;
    }

    xpc_object_t point(double x, double y) const {
        xpc_object_t value = xpc_dictionary_create(nullptr, nullptr, 0);
        xpc_dictionary_set_double(value, "x", point_width_ > 0 ? x / point_width_ : 0);
        xpc_dictionary_set_double(value, "y", point_height_ > 0 ? y / point_height_ : 0);
        return value;
    }

    static std::uint64_t contact_phase(bool& active, Direction direction) {
        if (direction == Direction::Up) { active = false; return 2; }
        if (active) return 1;
        active = true;
        return 0;
    }

    void send(const char* type, xpc_object_t payload) {
        xpc_connection_send_message(connection_, envelope(type, payload, false));
        ++sent_;
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        xpc_connection_send_barrier(connection_, ^{ dispatch_semaphore_signal(done); });
        dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));
    }

    double point_width_;
    double point_height_;
    xpc_connection_t connection_ = nullptr;
    bool contact_active_ = false;
    bool two_finger_active_ = false;
    std::uint64_t sent_ = 0;
    std::uint64_t drained_ = 0;
};

} // namespace

bool lookup_button(const std::string& name, HidButton& out) {
    if (name == "home") out = {static_cast<int>(ACEIndigoButtonSourceHome), 0x0C, 0x40};
    else if (name == "lock") out = {static_cast<int>(ACEIndigoButtonSourceLock), 0x0C, 0x30};
    else if (name == "side_button") out = {static_cast<int>(ACEIndigoButtonSourceSide), 0x0C, 0x30};
    else if (name == "siri") out = {static_cast<int>(ACEIndigoButtonSourceSiri), 0x0C, 0xCF};
    else if (name == "apple_pay") out = {static_cast<int>(ACEIndigoButtonSourceApplePay), 0, 0};
    else if (name == "volume_up") out = {-1, 0x0C, 0xE9};
    else if (name == "volume_down") out = {-1, 0x0C, 0xEA};
    else if (name == "play_pause") out = {-1, 0x0C, 0xCD};
    else return false;
    return true;
}

std::string default_transport() {
    return version_at_least(environment().core_simulator_version, kFirstDTUHIDVersion) ? "dtuhid" : "indigo";
}

std::unique_ptr<HidSession> open_hid_session(id device, double point_width, double point_height,
                                             const std::string& requested) {
    const std::string transport = requested.empty() ? default_transport() : requested;
    if (transport == "dtuhid") return std::make_unique<DtuhidSession>(device, point_width, point_height);
    if (transport == "indigo") return std::make_unique<IndigoSession>(device, point_width, point_height);
    throw SimError("BAD_REQUEST", "Unknown HID transport: " + transport);
}

} // namespace acecode::ios_simulator::helper
