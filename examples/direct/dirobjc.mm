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
#include <iostream>
#include <condition_variable>
#include <netinet/in.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <ifaddrs.h>
#include <string>

// Global handle for advertisement (for simplicity, only one at a time)
static DNSServiceRef g_advert_ref = nullptr;

// Struct to hold IP address and Wi-Fi flag
struct IPAddressResult {
    std::string ip;
    bool isWiFi;
};

IPAddressResult GetLocalIPAddress() {
    struct ifaddrs *ifaddr, *ifa;
    char ip[INET_ADDRSTRLEN] = {0};
    bool isWiFi = false;

    if (getifaddrs(&ifaddr) == -1) {
        std::cerr << "getifaddrs failed: " << strerror(errno) << std::endl;
        return {"", false};
    }

    // Debug: Log all interfaces
    std::cout << "Available interfaces:" << std::endl;
    for (ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr) continue;
        if (ifa->ifa_addr->sa_family == AF_INET) {
            inet_ntop(AF_INET, &((struct sockaddr_in *)ifa->ifa_addr)->sin_addr, ip, INET_ADDRSTRLEN);
            std::cout << "Interface: " << ifa->ifa_name << ", IP: " << ip
                      << ", Flags: " << (ifa->ifa_flags & IFF_LOOPBACK ? "LOOPBACK" : "")
                      << (ifa->ifa_flags & IFF_UP ? "UP" : "") << std::endl;
            ip[0] = '\0';
        }
    }

    // First pass: Look for en0 (Wi-Fi)
    for (ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr) continue;
        if (ifa->ifa_addr->sa_family == AF_INET &&
            !(ifa->ifa_flags & IFF_LOOPBACK) &&
            (ifa->ifa_flags & IFF_UP) &&
            strcmp(ifa->ifa_name, "en0") == 0) {
            struct sockaddr_in *sin = (struct sockaddr_in *)ifa->ifa_addr;
            inet_ntop(AF_INET, &sin->sin_addr, ip, INET_ADDRSTRLEN);
            isWiFi = true;
            std::cout << "Selected interface: " << ifa->ifa_name << ", IP: " << ip << " (Wi-Fi)" << std::endl;
            break;
        }
    }

    // Second pass: Fallback to any non-loopback, active interface
    if (ip[0] == '\0') {
        for (ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
            if (ifa->ifa_addr == nullptr) continue;
            if (ifa->ifa_addr->sa_family == AF_INET &&
                !(ifa->ifa_flags & IFF_LOOPBACK) &&
                (ifa->ifa_flags & IFF_UP)) {
                struct sockaddr_in *sin = (struct sockaddr_in *)ifa->ifa_addr;
                inet_ntop(AF_INET, &sin->sin_addr, ip, INET_ADDRSTRLEN);
                isWiFi = false;
                std::cout << "Selected fallback interface: " << ifa->ifa_name << ", IP: " << ip << " (Non-Wi-Fi)" << std::endl;
                break;
            }
        }
    }

    freeifaddrs(ifaddr);
    if (ip[0] == '\0') {
        std::cerr << "No valid IP found (en0 not available, no fallback)" << std::endl;
    }
    return {ip, isWiFi};
}

bool DIRECT_API AdvertiseBonjourService(const std::string& name, int port) {
    if (g_advert_ref) {
        DNSServiceRefDeallocate(g_advert_ref);
        g_advert_ref = nullptr;
    }

    // Get the local IP and Wi-Fi flag
    IPAddressResult ipResult = GetLocalIPAddress();
    if (ipResult.ip.empty()) {
        std::cerr << "Failed to retrieve a valid IP address for Bonjour advertising" << std::endl;
        return false;
    }

    // Require Wi-Fi for Bonjour
    if (!ipResult.isWiFi) {
        std::cerr << "Not on Wi-Fi (interface is not en0), skipping Bonjour advertising" << std::endl;
        return false;
    }

    // Sanitize service name
    std::string service_name = name;
    // Remove invalid characters (keep alphanumeric, hyphen, underscore)
    for (char& c : service_name) {
        if (!std::isalnum(c) && c != '-' && c != '_') {
            c = '_';
        }
    }
    // Trim to 63 bytes (Bonjour limit)
    if (service_name.length() > 63) {
        service_name = service_name.substr(0, 63);
    }
    // Fallback if empty or invalid
    if (service_name.empty()) {
        std::cerr << "Service name is empty or invalid after sanitization" << std::endl;
        return false;
    }

    // Get system hostname
    char sys_hostname[256];
    if (gethostname(sys_hostname, sizeof(sys_hostname)) != 0) {
        std::cerr << "gethostname failed: " << strerror(errno) << std::endl;
        sys_hostname[0] = '\0';
    }

    // Generate a unique .local hostname
    std::string hostname;
    std::string base_hostname = service_name; // Use service name as base (e.g., "RobotPhone")
    // Convert to lowercase and replace spaces/underscores for mDNS compatibility
    for (char& c : base_hostname) {
        if (c == ' ' || c == '_') {
            c = '-';
        } else {
            c = std::tolower(c);
        }
    }
    // Trim to 63 characters (mDNS hostname limit without .local)
    if (base_hostname.length() > 63) {
        base_hostname = base_hostname.substr(0, 63);
    }
    hostname = base_hostname + ".local"; // Append .local (e.g., "robotphone.local")

    // Validate system hostname and override if invalid
    std::string sys_hostname_str(sys_hostname);
    if (sys_hostname_str.empty() || sys_hostname_str == "localhost" || 
        sys_hostname_str.find(".local") == std::string::npos) {
        std::cout << "Invalid system hostname: " << (sys_hostname_str.empty() ? "empty" : sys_hostname_str) 
                  << ", using generated hostname: " << hostname << std::endl;
    } else {
        // Use system hostname if valid and ends with .local
        hostname = sys_hostname_str;
        if (hostname.find(".local") == std::string::npos) {
            hostname += ".local";
        }
    }

    // Create TXT record with IP
    std::string txt_record = "ip=" + ipResult.ip;
    unsigned char txt_buffer[256] = {0};
    uint16_t txt_len = 0;
    if (!txt_record.empty()) {
        size_t len = txt_record.length();
        if (len < 255) {
            txt_buffer[0] = static_cast<unsigned char>(len);
            memcpy(txt_buffer + 1, txt_record.c_str(), len);
            txt_len = len + 1;
        } else {
            std::cerr << "TXT record too long: " << txt_record << std::endl;
            txt_len = 0;
        }
    }

    // Log parameters for debugging
    std::cout << "Registering Bonjour service:" << std::endl;
    std::cout << "  Name: " << service_name << std::endl;
    std::cout << "  Type: _webrtcsays._tcp" << std::endl;
    std::cout << "  Port: " << port << std::endl;
    std::cout << "  Host: " << hostname << std::endl;
    std::cout << "  TXT: " << (txt_len ? txt_record : "none") << std::endl;

    // Advertise as _webrtcsays._tcp.local.
    DNSServiceErrorType err = DNSServiceRegister(
        &g_advert_ref,
        0, // flags
        0, // interface index (0 = all)
        service_name.c_str(),
        "_webrtcsays._tcp",
        nullptr, // domain (default local)
        hostname.c_str(),
        htons(port),
        txt_len,
        txt_len ? txt_buffer : nullptr,
        nullptr, // callback
        nullptr  // context
    );

    if (err != kDNSServiceErr_NoError) {
        std::cerr << "Bonjour advertise failed: " << err;
        switch (err) {
            case kDNSServiceErr_BadParam:
                std::cerr << " (Bad parameter)";
                break;
            case kDNSServiceErr_NameConflict:
                std::cerr << " (Name conflict)";
                break;
            case kDNSServiceErr_NoMemory:
                std::cerr << " (Out of memory)";
                break;
            default:
                std::cerr << " (Unknown error)";
                break;
        }
        std::cerr << std::endl;
        std::cerr << "Parameters: Name=" << service_name << ", Type=_webrtcsays._tcp, Port=" << port
                  << ", Host=" << hostname << ", TXT=" << (txt_len ? txt_record : "none") << std::endl;
        return false;
    }

    std::cout << "Bonjour advertised as '" << service_name << "' on port " << port << std::endl;
    std::cout << " Advertising on IP: " << ipResult.ip << " (" << (ipResult.isWiFi ? "Wi-Fi" : "Non-Wi-Fi") << ")" << std::endl;

    // Run the service in a background thread
    std::thread([]{
        if (g_advert_ref) {
            DNSServiceProcessResult(g_advert_ref);
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