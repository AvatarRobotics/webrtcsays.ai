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
    std::string selectedInterface;

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

    // Helper function to check if interface name looks like a network interface
    auto isNetworkInterface = [](const char* name) {
        // Check for common network interface patterns: en0, en1, en2, etc.
        return (strncmp(name, "en", 2) == 0 && strlen(name) >= 3 && isdigit(name[2]));
    };

    // First pass: Look for preferred network interfaces (en0, en1, etc.)
    for (ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr) continue;
        if (ifa->ifa_addr->sa_family == AF_INET &&
            !(ifa->ifa_flags & IFF_LOOPBACK) &&
            (ifa->ifa_flags & IFF_UP) &&
            isNetworkInterface(ifa->ifa_name)) {
            struct sockaddr_in *sin = (struct sockaddr_in *)ifa->ifa_addr;
            inet_ntop(AF_INET, &sin->sin_addr, ip, INET_ADDRSTRLEN);
            isWiFi = true;  // Assume en* interfaces are Wi-Fi/Ethernet (suitable for Bonjour)
            selectedInterface = ifa->ifa_name;
            std::cout << "Selected interface: " << ifa->ifa_name << ", IP: " << ip << " (Network)" << std::endl;
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
                selectedInterface = ifa->ifa_name;
                std::cout << "Selected fallback interface: " << ifa->ifa_name << ", IP: " << ip << " (Fallback)" << std::endl;
                break;
            }
        }
    }

    freeifaddrs(ifaddr);
    if (ip[0] == '\0') {
        std::cerr << "No valid IP found" << std::endl;
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

    // Prefer network interfaces for Bonjour, but allow fallback
    if (!ipResult.isWiFi) {
        std::cout << "Warning: Using fallback interface for Bonjour advertising (may have limited discoverability)" << std::endl;
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

    // Get system hostname for logging purposes
    char sys_hostname[256];
    if (gethostname(sys_hostname, sizeof(sys_hostname)) != 0) {
        std::cerr << "gethostname failed: " << strerror(errno) << std::endl;
        sys_hostname[0] = '\0';
    }
    
    std::string sys_hostname_str(sys_hostname);
    std::cout << "System hostname: " << (sys_hostname_str.empty() ? "empty" : sys_hostname_str) << std::endl;

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
    std::cout << "  Host: (system default)" << std::endl;
    std::cout << "  TXT: " << (txt_len ? txt_record : "none") << std::endl;

    // Advertise as _webrtcsays._tcp.local.
    // Use nullptr for hostname to let mDNS use the system's resolvable hostname
    DNSServiceErrorType err = DNSServiceRegister(
        &g_advert_ref,
        0, // flags
        0, // interface index (0 = all)
        service_name.c_str(),
        "_webrtcsays._tcp",
        nullptr, // domain (default local)
        nullptr, // hostname (use system default)
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
                  << ", Host=(system default), TXT=" << (txt_len ? txt_record : "none") << std::endl;
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
        std::cout << "[Resolve] Full name: " << (fullname ? fullname : "null") << std::endl;
        std::cout << "[Resolve] Host target: " << (hosttarget ? hosttarget : "null") << std::endl;
        std::cout << "[Resolve] Port: " << ntohs(port) << std::endl;
        
        // First try to extract IP from TXT record
        std::string ip_from_txt;
        if (txtLen > 0 && txtRecord) {
            // Parse TXT record to find ip= field
            const unsigned char* ptr = txtRecord;
            const unsigned char* end = txtRecord + txtLen;
            while (ptr < end) {
                uint8_t len = *ptr++;
                if (ptr + len <= end) {
                    std::string entry(reinterpret_cast<const char*>(ptr), len);
                    if (entry.substr(0, 3) == "ip=") {
                        ip_from_txt = entry.substr(3);
                        std::cout << "[Resolve] Found IP in TXT: " << ip_from_txt << std::endl;
                        break;
                    }
                }
                ptr += len;
            }
        }
        
        if (!ip_from_txt.empty()) {
            // Use IP from TXT record
            ctx->found_ip = ip_from_txt;
            ctx->found_port = ntohs(port);
            ctx->found = true;
            ctx->cv.notify_all();
            std::cout << "[Resolve] Success via TXT: " << ctx->found_ip << ":" << ctx->found_port << std::endl;
        } else if (hosttarget) {
            // Fallback: resolve hosttarget to IP
            std::cout << "[Resolve] Attempting to resolve hostname: " << hosttarget << std::endl;
            struct addrinfo hints = {0, AF_INET, SOCK_STREAM, 0, 0, 0, 0, nullptr};
            // hints.ai_family = AF_INET;
            // hints.ai_socktype = SOCK_STREAM;
            struct addrinfo* res = nullptr;
            if (getaddrinfo(hosttarget, nullptr, &hints, &res) == 0 && res) {
                char ipstr[INET_ADDRSTRLEN] = {0};
                struct sockaddr_in* s = (struct sockaddr_in*)res->ai_addr;
                inet_ntop(AF_INET, &s->sin_addr, ipstr, sizeof(ipstr));
                ctx->found_ip = ipstr;
                ctx->found_port = ntohs(port);
                freeaddrinfo(res);
                ctx->found = true;
                ctx->cv.notify_all();
                std::cout << "[Resolve] Success via hostname: " << ctx->found_ip << ":" << ctx->found_port << std::endl;
            } else {
                std::cout << "[Resolve] Failed to resolve hostname: " << hosttarget << std::endl;
            }
        }
    } else {
        std::cout << "[Resolve] Error: " << errorCode << std::endl;
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
    
    if (errorCode != kDNSServiceErr_NoError) {
        std::cout << "[Browse] Error: " << errorCode << std::endl;
        return;
    }
    
    if (!ctx || !serviceName) {
        std::cout << "[Browse] Invalid context or service name" << std::endl;
        return;
    }
    
    std::cout << "[Browse] Found service: '" << serviceName << "' in domain: " << (replyDomain ? replyDomain : "null") << std::endl;
    std::cout << "[Browse] Looking for: '" << ctx->target_name << "'" << std::endl;
    
    // Check if this is the service we're looking for
    bool isTargetService = (ctx->target_name == serviceName);
    
    if (isTargetService || ctx->target_name.empty()) {
        std::cout << "[Browse] " << (isTargetService ? "Target service found" : "Resolving first available service") << std::endl;
        
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
            std::cout << "[Browse] Starting resolution..." << std::endl;
            // Process the resolve operation with timeout
            auto start_time = std::chrono::steady_clock::now();
            while (!ctx->found && 
                   std::chrono::steady_clock::now() - start_time < std::chrono::seconds(5)) {
                DNSServiceErrorType processErr = DNSServiceProcessResult(resolveRef);
                if (processErr != kDNSServiceErr_NoError) {
                    std::cout << "[Browse] Process result error: " << processErr << std::endl;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            DNSServiceRefDeallocate(resolveRef);
            
            if (ctx->found) {
                std::cout << "[Browse] Resolution completed successfully" << std::endl;
            } else {
                std::cout << "[Browse] Resolution timed out" << std::endl;
            }
        } else {
            std::cout << "[Browse] DNSServiceResolve failed: " << err << std::endl;
        }
    } else {
        std::cout << "[Browse] Skipping service (not target): " << serviceName << std::endl;
    }
}

bool DIRECT_API DiscoverBonjourService(const std::string& name, std::string& out_ip, int& out_port, int timeout_seconds) {
    std::cout << "[Discovery] Starting Bonjour discovery for: '" << name << "'" << std::endl;
    
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
        std::cerr << "[Discovery] Bonjour browse failed: " << err << std::endl;
        return false;
    }
    
    std::cout << "[Discovery] Browse started, waiting for results..." << std::endl;
    
    // Wait for result or timeout
    int effective_timeout = timeout_seconds > 0 ? timeout_seconds : 15;
    auto start = std::chrono::steady_clock::now();
    auto timeout_duration = std::chrono::seconds(effective_timeout);
    
    while (!ctx.found && std::chrono::steady_clock::now() - start < timeout_duration) {
        DNSServiceErrorType processErr = DNSServiceProcessResult(browseRef);
        if (processErr != kDNSServiceErr_NoError) {
            std::cout << "[Discovery] Process result error: " << processErr << std::endl;
            break;
        }
        
        // Short sleep to prevent busy waiting
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        
        // Check if we found something
        if (ctx.found) {
            std::cout << "[Discovery] Service found and resolved!" << std::endl;
            break;
        }
    }
    
    DNSServiceRefDeallocate(browseRef);
    
    if (ctx.found) {
        out_ip = ctx.found_ip;
        out_port = ctx.found_port;
        std::cout << "[Discovery] Success: " << out_ip << ":" << out_port << std::endl;
        return true;
    } else {
        std::cout << "[Discovery] Timeout: No service found within " << effective_timeout << " seconds" << std::endl;
        return false;
    }
}

#endif // #if TARGET_OS_IOS || TARGET_OS_OSX