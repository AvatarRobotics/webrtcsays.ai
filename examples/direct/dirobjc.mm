/*
 *  (c) 2025, wilddolphin2022
 *  For WebRTCsays.ai project
 *  https://github.com/wilddolphin2022
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#import "direct.h"

#if TARGET_OS_IOS && defined(__OBJC__)
#include <memory>

#import "sdk/objc/base/RTCVideoCapturer.h"
#import "sdk/objc/components/renderer/metal/RTCMTLVideoView.h"
#import "sdk/objc/native/src/objc_video_track_source.h"
#import "sdk/objc/native/src/objc_video_renderer.h"

// Convenience shim: take an Obj-C RTCVideoCapturer and wrap it into WebRTC's
// ObjCVideoTrackSource, then inject into DirectApplication.
void DirectApplication::SetVideoCapturer(RTCVideoCapturer* capturer) {
  // Create adapter that implements RTCVideoCapturerDelegate
  RTCObjCVideoSourceAdapter* adapter = [[RTCObjCVideoSourceAdapter alloc] init];
  // Point capturer at our adapter
  capturer.delegate = adapter;
  // Wrap adapter in a native VideoTrackSource
  auto native_source = rtc::make_ref_counted<webrtc::ObjCVideoTrackSource>(adapter);
  // Hand it to the C++ engine
  SetVideoSource(native_source);
}

void DirectApplication::SetVideoRenderer(RTCMTLVideoView* renderer) {
  // Wrap the provided Obj-C view into a native VideoSink
  std::unique_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> native_sink =
      std::make_unique<webrtc::ObjCVideoRenderer>(renderer);
  // Hand it to the C++ engine
  SetVideoSink(std::move(native_sink));
}
#endif  // #if TARGET_OS_IOS && defined(__OBJC__)

#if TARGET_OS_IOS || TARGET_OS_OSX

// --- Bonjour (mDNS/DNS-SD) advertisement and discovery for C++ ---
// Only works on macOS/iOS. Used by AdvertiseBonjourService/DiscoverBonjourService in bonjour.h

#include "bonjour.h"
#include <dns_sd.h>
#include <thread>
#include <chrono>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <iostream>
#include <netdb.h>

// Global handle for advertisement (for simplicity, only one at a time)
static DNSServiceRef g_advert_ref = nullptr;

bool DIRECT_API AdvertiseBonjourService(const std::string& name, int port) {
    if (g_advert_ref) {
        DNSServiceRefDeallocate(g_advert_ref);
        g_advert_ref = nullptr;
    }
    // Advertise as _webrtcsays._tcp. local.
    DNSServiceErrorType err = DNSServiceRegister(
        &g_advert_ref,
        0, // flags
        0, // interface index (0 = all)
        name.c_str(), // service name
        "_webrtcsays._tcp", // service type
        nullptr, // domain (default local)
        nullptr, // host (default)
        htons(port), // port in network byte order
        0, // txtLen
        nullptr, // txtRecord
        nullptr, // callback
        nullptr  // context
    );
    if (err != kDNSServiceErr_NoError) {
        std::cerr << "Bonjour advertise failed: " << err << std::endl;
        return false;
    }
    // Run the service in a background thread
    std::thread([]{
        if (g_advert_ref) {
            DNSServiceProcessResult(g_advert_ref); // This blocks
        }
    }).detach();
    return true;
}

// Helper for discovery
struct BonjourDiscoveryContext {
    std::string target_name;
    std::string found_ip;
    int found_port = 0;
    std::atomic<bool> found{false};
    std::mutex mtx;
    std::condition_variable cv;
};

static void DNSSD_API resolve_callback(
    DNSServiceRef sdRef,
    DNSServiceFlags flags,
    uint32_t interfaceIndex,
    DNSServiceErrorType errorCode,
    const char *fullname,
    const char *hosttarget,
    uint16_t port,
    uint16_t txtLen,
    const unsigned char *txtRecord,
    void *context)
{
    BonjourDiscoveryContext* ctx = static_cast<BonjourDiscoveryContext*>(context);
    if (errorCode == kDNSServiceErr_NoError) {
        // Resolve hosttarget to IP
        struct addrinfo hints = {0,0,0,0,0,0,0,0}, *res = nullptr;
        hints.ai_family = AF_INET;
        if (getaddrinfo(hosttarget, nullptr, &hints, &res) == 0 && res) {
            char ipstr[INET_ADDRSTRLEN] = {0};
            struct sockaddr_in* s = (struct sockaddr_in*)res->ai_addr;
            inet_ntop(AF_INET, &s->sin_addr, ipstr, sizeof(ipstr));
            ctx->found_ip = ipstr;
            ctx->found_port = ntohs(port);
            freeaddrinfo(res);
            ctx->found = true;
            ctx->cv.notify_all();
        }
    }
}

static void DNSSD_API browse_callback(
    DNSServiceRef sdRef,
    DNSServiceFlags flags,
    uint32_t interfaceIndex,
    DNSServiceErrorType errorCode,
    const char *serviceName,
    const char *regtype,
    const char *replyDomain,
    void *context)
{
    BonjourDiscoveryContext* ctx = static_cast<BonjourDiscoveryContext*>(context);
    // Debug: print both names, lengths, and hex
    if (serviceName) {
        std::cout << "[Bonjour] Looking for: '" << ctx->target_name << "' (len=" << ctx->target_name.length() << ")\n";
        std::cout << "[Bonjour] Found:      '" << serviceName << "' (len=" << strlen(serviceName) << ")\n";
        // Print hex for both
        std::cout << "[Bonjour] Target hex: ";
        for (char c : ctx->target_name) std::cout << std::hex << ((int)(unsigned char)c) << " ";
        std::cout << "\n[Bonjour] Found hex:  ";
        for (size_t i = 0; i < strlen(serviceName); ++i) std::cout << std::hex << ((int)(unsigned char)serviceName[i]) << " ";
        std::cout << std::dec << std::endl;
    }
    if (errorCode == kDNSServiceErr_NoError && ctx && serviceName) {
        // For testing: always try to resolve the first service found
        if (ctx->target_name != serviceName) {
            std::cout << "[Bonjour] WARNING: Name mismatch, but resolving anyway for test." << std::endl;
        }
        DNSServiceRef resolveRef = nullptr;
        DNSServiceErrorType err = DNSServiceResolve(
            &resolveRef,
            0,
            interfaceIndex,
            serviceName,
            regtype,
            replyDomain,
            resolve_callback,
            context);
        if (err == kDNSServiceErr_NoError) {
            DNSServiceProcessResult(resolveRef);
            DNSServiceRefDeallocate(resolveRef);
        }
    }
}

bool DIRECT_API DiscoverBonjourService(const std::string& name, std::string& out_ip, int& out_port, int timeout_seconds) {
    BonjourDiscoveryContext ctx;
    ctx.target_name = name;
    DNSServiceRef browseRef = nullptr;
    DNSServiceErrorType err = DNSServiceBrowse(
        &browseRef,
        0,
        0,
        "_webrtcsays._tcp",
        nullptr,
        browse_callback,
        &ctx);
    if (err != kDNSServiceErr_NoError) {
        std::cerr << "Bonjour browse failed: " << err << std::endl;
        return false;
    }
    // Wait for result or timeout
    std::unique_lock<std::mutex> lock(ctx.mtx);
    int effective_timeout = timeout_seconds > 0 ? timeout_seconds : 10;
    //ctx.cv.wait_for(lock, std::chrono::seconds(effective_timeout), [&ctx]{ return ctx.found.load(); });
    auto start = std::chrono::steady_clock::now();
    while (!ctx.found && std::chrono::steady_clock::now() - start < std::chrono::seconds(effective_timeout)) {
        DNSServiceProcessResult(browseRef);
    }
    DNSServiceRefDeallocate(browseRef);

    DNSServiceRefDeallocate(browseRef);
    if (ctx.found) {
        out_ip = ctx.found_ip;
        out_port = ctx.found_port;
        return true;
    }
    return false;
}

#endif // #if TARGET_OS_IOS || TARGET_OS_OSX