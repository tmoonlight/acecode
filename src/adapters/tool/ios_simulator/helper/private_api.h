// Private CoreSimulator / SimulatorKit / AccessibilityPlatformTranslation
// surface used by acecode-ios-sim. Every class is resolved at runtime with
// NSClassFromString and messaged through these protocols, so no link-time
// symbol pins the helper to one Xcode layout.
//
// The Indigo wire structures and selector shapes are adapted from Meta's
// idb (FBSimulatorControl, PrivateHeaders/SimulatorApp/Indigo.h), MIT License,
// Copyright (c) Meta Platforms, Inc. and affiliates. See
// src/adapters/tool/ios_simulator/helper/LICENSE-idb.txt.

#pragma once

#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#include <xpc/xpc.h>

// ---------------------------------------------------------------------------
// CoreSimulator

@protocol ACESimServiceContextFactory
- (id)sharedServiceContextForDeveloperDir:(NSString *)developerDir error:(NSError **)error;
@end

@protocol ACESimServiceContext
- (id)defaultDeviceSetWithError:(NSError **)error;
@end

@protocol ACESimDeviceSet
@property (nonatomic, readonly) NSArray *devices;
@end

@protocol ACESimDeviceType
@property (nonatomic, assign) CGSize mainScreenSize;
@property (nonatomic, assign) float mainScreenScale;
@property (nonatomic, readonly) int productFamilyID;
@property (nonatomic, copy) NSString *name;
@end

@protocol ACESimRuntime
@property (nonatomic, copy) NSString *versionString;
@property (nonatomic, copy) NSString *identifier;
@property (nonatomic, copy) NSString *name;
@end

@protocol ACESimDevice
@property (nonatomic, readonly, copy) NSString *name;
@property (nonatomic, readonly) unsigned long long state;
@property (nonatomic, copy) NSUUID *UDID;
@property (nonatomic, readonly) id runtime;
@property (nonatomic, readonly) id deviceType;
@property (nonatomic, retain) id io;
- (unsigned int)lookup:(NSString *)serviceName error:(NSError **)error;
- (void)sendAccessibilityRequestAsync:(id)request
                      completionQueue:(dispatch_queue_t)queue
                    completionHandler:(void (^)(id response))handler;
- (BOOL)setHardwareKeyboardEnabled:(BOOL)enabled keyboardType:(unsigned char)type error:(NSError **)error;
@end

@protocol ACESimDeviceIOClient
- (id)ioPorts;
@end

@protocol ACESimDeviceIOPort
@property (nonatomic, readonly) id descriptor;
@end

@protocol ACESimDisplayDescriptorState
@property (nonatomic, readonly) unsigned short displayClass;
@end

@protocol ACESimDisplayRenderable
@property (nonatomic, readonly) CGSize displaySize;
@property (nonatomic, readonly) id ioSurface;
@property (nonatomic, readonly) id framebufferSurface;
- (id)state;
- (void)registerCallbackWithUUID:(NSUUID *)uuid ioSurfaceChangeCallback:(void (^)(id))callback;
- (void)registerCallbackWithUUID:(NSUUID *)uuid ioSurfacesChangeCallback:(void (^)(id))callback;
- (void)registerCallbackWithUUID:(NSUUID *)uuid damageRectanglesCallback:(void (^)(NSArray *))callback;
- (void)unregisterIOSurfaceChangeCallbackWithUUID:(NSUUID *)uuid;
- (void)unregisterIOSurfacesChangeCallbackWithUUID:(NSUUID *)uuid;
- (void)unregisterDamageRectanglesCallbackWithUUID:(NSUUID *)uuid;
@end

@protocol ACESimScreen
- (void)registerScreenCallbacksWithUUID:(NSUUID *)uuid
                          callbackQueue:(dispatch_queue_t)queue
                          frameCallback:(void (^)(void))frame
                surfacesChangedCallback:(void (^)(id, id))surfaces
              propertiesChangedCallback:(void (^)(id))properties;
- (void)unregisterScreenCallbacksWithUUID:(NSUUID *)uuid;
@end

// ---------------------------------------------------------------------------
// SimulatorKit (runtime-only Swift class "SimulatorKit.SimDeviceLegacyHIDClient")

@protocol ACESimDeviceLegacyHIDClient
- (id)initWithDevice:(id)device error:(NSError **)error;
- (void)sendWithMessage:(void *)message
           freeWhenDone:(BOOL)freeWhenDone
        completionQueue:(dispatch_queue_t)queue
             completion:(void (^)(NSError *))completion;
@end

// ---------------------------------------------------------------------------
// AccessibilityPlatformTranslation

typedef id (^ACEAXPTranslationCallback)(id request);

@protocol ACEAXPTranslatorFactory
- (id)sharedInstance;
- (id)sharediOSInstance;
@end

@protocol ACEAXPTranslator
@property (nonatomic) BOOL supportsDelegateTokens;
@property (nonatomic) BOOL accessibilityEnabled;
@property (nonatomic, weak) id bridgeTokenDelegate;
- (id)frontmostApplicationWithDisplayId:(unsigned int)displayId bridgeDelegateToken:(NSString *)token;
- (id)objectAtPoint:(CGPoint)point displayId:(unsigned int)displayId bridgeDelegateToken:(NSString *)token;
- (id)macPlatformElementFromTranslation:(id)translation;
@end

@protocol ACEAXPTranslationObject
@property (nonatomic, copy) NSString *bridgeDelegateToken;
@property (nonatomic) int pid;
@end

@protocol ACEAXPPlatformElement
@property (nonatomic, readonly) id translation;
- (NSArray *)accessibilityChildren;
- (NSString *)accessibilityRole;
- (NSString *)accessibilitySubrole;
- (NSString *)accessibilityLabel;
- (id)accessibilityValue;
- (NSString *)accessibilityIdentifier;
- (NSString *)accessibilityTitle;
- (NSString *)accessibilityHelp;
- (NSString *)accessibilityRoleDescription;
- (NSString *)accessibilityPlaceholderValue;
- (NSRect)accessibilityFrame;
- (BOOL)isAccessibilityEnabled;
- (BOOL)isAccessibilityFocused;
- (BOOL)isAccessibilitySelected;
- (id)accessibilityAttributeValue:(NSString *)attribute;
// Private batch read: one translator round trip for many attributes.
- (NSDictionary *)accessibilityMultipleAttributes:(NSArray *)names;
@end

// ---------------------------------------------------------------------------
// Indigo HID wire format (#pragma pack(4); see idb Indigo.h for provenance).

#pragma pack(push, 4)

typedef struct {
  unsigned int msgh_bits;
  unsigned int msgh_size;
  unsigned int msgh_remote_port;
  unsigned int msgh_local_port;
  unsigned int msgh_voucher_port;
  int msgh_id;
} ACEMachMessageHeader;

typedef struct {
  double field1;
  double field2;
  double field3;
  double field4;
} ACEIndigoQuad;

typedef struct {
  unsigned int field1;
  unsigned int field2;
  unsigned int eventMask;
  double xRatio;
  double yRatio;
  double field6;
  double field7;
  double field8;
  unsigned int range;
  unsigned int touch;
  unsigned int field11;
  unsigned int field12;
  unsigned int field13;
  double field14;
  double field15;
  double field16;
  double field17;
  double field18;
} ACEIndigoTouch;

typedef struct {
  unsigned int eventSource;
  unsigned int eventType;
  unsigned int eventTarget;
  unsigned int keyCode;
  unsigned int field5;
  unsigned int usagePage;
} ACEIndigoButton;

typedef struct {
  ACEIndigoQuad dpad;
  ACEIndigoQuad face;
  ACEIndigoQuad shoulder;
  ACEIndigoQuad joystick;
} ACEIndigoGameController;

typedef union {
  ACEIndigoTouch touch;
  ACEIndigoButton button;
  ACEIndigoGameController gameController;
} ACEIndigoEvent;

typedef struct {
  unsigned int eventKind;
  unsigned long long timestamp;
  unsigned int field3;
  ACEIndigoEvent event;
} ACEIndigoPayload;

typedef struct {
  ACEMachMessageHeader header;
  unsigned int innerSize;
  unsigned char eventType;
  ACEIndigoPayload payload;
} ACEIndigoMessage;

#pragma pack(pop)

enum : unsigned int {
  ACEIndigoEventTypeTouch = 2,
  ACEIndigoButtonEventTypeDown = 1,
  ACEIndigoButtonEventTypeUp = 2,
  ACEIndigoButtonTargetHardware = 0x33,
  ACEIndigoButtonTargetDigitizer = 0x32,
  ACEIndigoMouseTarget = 0x32,
  ACEIndigoButtonSourceHome = 0x0,
  ACEIndigoButtonSourceLock = 0x1,
  ACEIndigoButtonSourceSide = 0xbb8,
  ACEIndigoButtonSourceSiri = 0x400002,
  ACEIndigoButtonSourceApplePay = 0x1f4,
};

// Offsets that the packed C layout cannot express (SimulatorKit lays later
// payloads out at a 0xA0 stride; the hand-built single touch uses 0x90).
static const size_t ACEIndigoPayloadStride = sizeof(ACEIndigoPayload);
static const size_t ACEIndigoSecondPayloadWireOffset = 0xC0;
static const size_t ACEIndigoPayloadWireStride = 0xA0;

typedef ACEIndigoMessage *(*ACEIndigoMessageForButtonFn)(int source, int direction, int target);
typedef ACEIndigoMessage *(*ACEIndigoMessageForKeyboardArbitraryFn)(int usage, int direction);
typedef ACEIndigoMessage *(*ACEIndigoMessageForHIDArbitraryFn)(int target, unsigned int page,
                                                                unsigned int usage, int direction);
typedef ACEIndigoMessage *(*ACEIndigoMessageForMouseNSEventFn)(CGPoint *point, CGPoint *point2,
                                                                unsigned int target, unsigned long eventType,
                                                                CGSize size, unsigned int edge);

// libxpc simulator bridging (resolved with dlsym).
typedef xpc_object_t (*ACEXPCEndpointFromPortFn)(mach_port_t port, uint64_t a, uint64_t b);
typedef xpc_connection_t (*ACEXPCConnectionFromEndpointFn)(xpc_object_t endpoint);
typedef void (*ACEXPCEnableSim2HostFn)(xpc_connection_t connection);
