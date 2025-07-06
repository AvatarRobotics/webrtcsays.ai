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

#include <string>
#include <memory>
#include <cstring> // For memset
#include <unistd.h> // For close
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h> // For errno
#include <chrono>
#include <thread>
#include <sstream> // Added for building INIT JSON
#include <api/units/time_delta.h> // For webrtc::TimeDelta

#include "direct.h"

#if TARGET_OS_IOS || TARGET_OS_OSX
#include "bonjour.h"
#endif // #if TARGET_OS_IOS || TARGET_OS_OSX

// Function to parse IP address and port from a string in the format "IP:PORT"
// From option.cc
bool ParseIpAndPort(const std::string& ip_port, std::string& ip, int& port);

// DirectCaller Implementation
DirectCaller::DirectCaller(Options opts)
    : DirectPeer(opts)
{ 
#if TARGET_OS_IOS || TARGET_OS_OSX
    // Bonjour discovery for iOS and macOS
    if (opts.bonjour && !opts.bonjour_name.empty()) {
        std::string ip;
        int port = 0;
        if (DiscoverBonjourService(opts.bonjour_name, ip, port)) {
            remote_addr_ = rtc::SocketAddress(ip, port);
            RTC_LOG(LS_INFO) << "Bonjour discovery found service " << opts.bonjour_name << " at " << ip << ":" << port;
        } else {
            ParseIpAndPort(opts.address, ip, port);
            remote_addr_ = rtc::SocketAddress(ip, port);
        }
    } else {
        std::string ip; int port = 0;
        ParseIpAndPort(opts.address, ip, port);
        remote_addr_ = rtc::SocketAddress(ip, port);
    }
#else
    std::string ip;
    int port = 0;    
    ParseIpAndPort(opts.address, ip, port);
    remote_addr_ = rtc::SocketAddress(ip, port);
#endif // #if TARGET_OS_IOS || TARGET_OS_OSX
}

DirectCaller::~DirectCaller() {
    // tcp_socket_ cleanup is handled by DirectApplication base class destructor/Cleanup
}

bool DirectCaller::Connect(const char* ip, int port) {
    remote_addr_ = rtc::SocketAddress(ip, port);
    return Connect();
}

#if TARGET_OS_IOS || TARGET_OS_OSX
bool DirectCaller::ConnectWithBonjourName(const char* bonjour_name) {
    std::string ip;
    int port = 0;
    if (DiscoverBonjourService(bonjour_name, ip, port)) {
        remote_addr_ = rtc::SocketAddress(ip, port);
    }
    return Connect();
}
#endif // #if TARGET_OS_IOS || TARGET_OS_OSX

bool DirectCaller::Connect() {
    auto task = [this]() -> bool {
        int raw_socket = -1; // will be set upon successful connection

        // Setup local address
        struct sockaddr_in local_addr;
        ::memset(&local_addr, 0, sizeof(local_addr));
        local_addr.sin_family = AF_INET;
        local_addr.sin_port = 0;
        local_addr.sin_addr.s_addr = INADDR_ANY;

        // Setup remote address
        struct sockaddr_in remote_addr;
        ::memset(&remote_addr, 0, sizeof(remote_addr));
        remote_addr.sin_family = AF_INET;
        remote_addr.sin_port = htons(remote_addr_.port());
        ::inet_pton(AF_INET, remote_addr_.ipaddr().ToString().c_str(), &remote_addr.sin_addr);

        RTC_LOG(LS_INFO) << "Attempting to connect to " << remote_addr_.ToString();

        const int kMaxConnectAttempts = 20; // allow up to ~1 minute total wait
        const int kInitialDelayMs = 100; // first retry delay

        int attempt = 0;
        int raw_socket_current = -1;

        while (attempt < kMaxConnectAttempts) {
            // Create a fresh socket for each attempt to avoid bad state after a failed connect.
            raw_socket_current = ::socket(AF_INET, SOCK_STREAM, 0);
            if (raw_socket_current < 0) {
                RTC_LOG(LS_ERROR) << "Failed to create raw socket on attempt " << (attempt + 1) << ", errno: " << strerror(errno);
                return false;
            }

            // Bind to ephemeral local port
            if (::bind(raw_socket_current, reinterpret_cast<struct sockaddr*>(&local_addr), sizeof(local_addr)) < 0) {
                RTC_LOG(LS_ERROR) << "Failed to bind raw socket on attempt " << (attempt + 1) << ", errno: " << strerror(errno);
                ::close(raw_socket_current);
                return false;
            }

            if (::connect(raw_socket_current, reinterpret_cast<struct sockaddr*>(&remote_addr), sizeof(remote_addr)) == 0) {
                raw_socket = raw_socket_current; // success
                break;
            }

            int err = errno;
            RTC_LOG(LS_WARNING) << "Connect attempt " << (attempt + 1) << " failed (" << strerror(err) << ")";
            ::close(raw_socket_current);

            ++attempt;
            if (attempt >= kMaxConnectAttempts) {
                RTC_LOG(LS_ERROR) << "Failed to connect raw socket after " << kMaxConnectAttempts << " attempts.";
                return false;
            }

            // Exponential back-off, cap to 4 seconds
            int delay_ms = kInitialDelayMs * (1 << std::min(attempt, 5));
            delay_ms = std::min(delay_ms, 4000);
            RTC_LOG(LS_INFO) << "Retrying connect in " << delay_ms << " ms";
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        }

        RTC_LOG(LS_INFO) << "Raw socket (" << raw_socket << ") connected successfully";

        // Wrap the connected socket using DirectApplication::WrapSocket to track it
        auto* wrapped_socket = WrapSocket(raw_socket); // Use inherited WrapSocket
        if (!wrapped_socket) {
            RTC_LOG(LS_ERROR) << "Failed to wrap socket, errno: " << strerror(errno);
            ::close(raw_socket);
            return false;
        }

        // Use inherited tcp_socket_
        tcp_socket_.reset(new rtc::AsyncTCPSocket(wrapped_socket));
        tcp_socket_->RegisterReceivedPacketCallback(
            [this](rtc::AsyncPacketSocket* socket, const rtc::ReceivedPacket& packet) {
                // Access data using packet.payload()
                OnMessage(socket, 
                          reinterpret_cast<const unsigned char*>(packet.payload().data()), 
                          packet.payload().size(), 
                          packet.source_address());
            });
        tcp_socket_->SubscribeCloseEvent("caller_close_event",
            [this](rtc::AsyncPacketSocket* socket, int err) {
                // Access data using packet.payload()
                OnClose(socket);
            });
        OnConnect(tcp_socket_.get());

        return true;
    };
    return network_thread()->BlockingCall(std::move(task));
}

void DirectCaller::OnConnect(rtc::AsyncPacketSocket* socket) {
    RTC_LOG(LS_INFO) << "Connected to " << remote_addr_.ToString();
    
    // Reset handshake tracking state for a fresh connection
    hello_attempts_ = 0;
    welcome_received_ = false;

    // Kick-off HELLO (with automatic retries)
    SendHelloWithRetry();
}

void DirectCaller::OnMessage(rtc::AsyncPacketSocket* socket,
                           const unsigned char* data,
                           size_t len,
                           const rtc::SocketAddress& remote_addr) {
    std::string message(reinterpret_cast<const char*>(data), len);
    RTC_LOG(LS_INFO) << "Caller received: " << message;

    // Allow for cases where multiple messages arrive concatenated in one TCP chunk.
    if (message.find("WELCOME") == 0) {
        // Stop further HELLO retries – handshake completed successfully.
        welcome_received_ = true;

        // Build INIT JSON with capability / option hints
        std::ostringstream oss;
        oss << "{";
        oss << "\"agent\":\"" << init_agent() << "\"";  // from DirectApplication preference
        // Include important runtime flags from opts_
        oss << ",\"encryption\":" << (opts_.encryption ? "true" : "false");
        oss << ",\"video\":" << (opts_.video ? "true" : "false");
        if (!opts_.llama_model.empty()) {
            oss << ",\"llama_model\":\"" << opts_.llama_model << "\"";
        }
        if (!opts_.llava_mmproj.empty()) {
            oss << ",\"llava_mmproj\":\"" << opts_.llava_mmproj << "\"";
        }
        if (!opts_.room_name.empty()) {
            oss << ",\"room_name\":\"" << opts_.room_name << "\"";
        }
        if (!opts_.language.empty()) {
            oss << ",\"language\":\"" << opts_.language << "\"";
        }
        oss << "}";

        std::string init_payload = "INIT:" + oss.str();
        SendMessage(init_payload);
    } 
    else if (message.find("WAITING") == 0) {
        Start();
    } 
    else if (message == "OK") {
        // Remote acknowledged our CANCEL. Close PeerConnection but keep
        // worker/signaling threads running so that we can place another
        // call without having to re-create the whole DirectCaller instance.
        ShutdownInternal();

        // Close and discard the obsolete TCP socket so that any subsequent
        // WELCOME/INIT handshake for the next call is delivered to the new
        // socket created by the next Connect().
        if (tcp_socket_) {
            tcp_socket_->Close();
            tcp_socket_.reset();
        }
    } else {
        HandleMessage(socket, message, remote_addr);
    }
}

void DirectCaller::Disconnect() {
    RTC_LOG(LS_INFO) << "Caller signaling disconnect, sending CANCEL due to connection timeout.";
    // Update timestamp *before* signaling/shutdown
    last_disconnect_time_ = std::chrono::steady_clock::now(); 
    if (SendMessage("CANCEL")) { // Send CANCEL to signal disconnect without shutdown
        RTC_LOG(LS_INFO) << "CANCEL message sent successfully.";
    } else {
        RTC_LOG(LS_WARNING) << "Failed to send CANCEL message.";
    }
    // Initiate the PeerConnection close process directly via the virtual method
    RTC_LOG(LS_INFO) << "Calling ShutdownInternal to close PeerConnection.";
    ShutdownInternal(); 
    // Reset state to allow for a new connection
    tcp_socket_.reset();
    RTC_LOG(LS_INFO) << "Caller state reset for new connection.";
}

// -----------------------------------------------------------------------------
// HELLO retry logic – ensures the callee receives a handshake even if the very
// first packet races ahead of the callee's read callback registration.
// -----------------------------------------------------------------------------

void DirectCaller::SendHelloWithRetry() {
    // If we already succeeded or exceeded max attempts, stop.
    if (welcome_received_ || hello_attempts_ >= kMaxHelloAttempts) {
        return;
    }

    // Send HELLO now.
    if (SendMessage("HELLO")) {
        RTC_LOG(LS_INFO) << "HELLO attempt " << (hello_attempts_ + 1)
                         << " sent successfully";
    } else {
        RTC_LOG(LS_WARNING) << "Failed to send HELLO attempt " << (hello_attempts_ + 1);
    }

    ++hello_attempts_;

    // Schedule next retry if necessary.
    if (!welcome_received_ && hello_attempts_ < kMaxHelloAttempts) {
        auto* nt = network_thread();
        if (nt) {
            nt->PostDelayedTask([this]() {
                this->SendHelloWithRetry();
            }, webrtc::TimeDelta::Millis(200));  // retry after 200 ms
        }
    }
}
