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

#include "direct.h"

// Function to parse IP address and port from a string in the format "IP:PORT"
// From option.cc
bool ParseIpAndPort(const std::string& ip_port, std::string& ip, int& port);

// DirectCaller Implementation
DirectCaller::DirectCaller(Options opts)
    : DirectPeer(opts)
{ 
    std::string ip; int port = 0;
    ParseIpAndPort(opts.address, ip, port); 
    remote_addr_ = rtc::SocketAddress(ip, port);
}

DirectCaller::~DirectCaller() {
    // tcp_socket_ cleanup is handled by DirectApplication base class destructor/Cleanup
}

bool DirectCaller::Connect() {
    auto task = [this]() -> bool {
        // Create raw socket
        int raw_socket = ::socket(AF_INET, SOCK_STREAM, 0);
        if (raw_socket < 0) {
            RTC_LOG(LS_ERROR) << "Failed to create raw socket, errno: " << strerror(errno);
            return false;
        }

        // Setup local address
        struct sockaddr_in local_addr;
        ::memset(&local_addr, 0, sizeof(local_addr));
        local_addr.sin_family = AF_INET;
        local_addr.sin_port = 0;
        local_addr.sin_addr.s_addr = INADDR_ANY;

        // Bind
        if (::bind(raw_socket, reinterpret_cast<struct sockaddr*>(&local_addr), sizeof(local_addr)) < 0) {
            RTC_LOG(LS_ERROR) << "Failed to bind raw socket, errno: " << strerror(errno);
            ::close(raw_socket);
            return false;
        }

        // Setup remote address
        struct sockaddr_in remote_addr;
        ::memset(&remote_addr, 0, sizeof(remote_addr));
        remote_addr.sin_family = AF_INET;
        remote_addr.sin_port = htons(remote_addr_.port());
        ::inet_pton(AF_INET, remote_addr_.ipaddr().ToString().c_str(), &remote_addr.sin_addr);

        RTC_LOG(LS_INFO) << "Attempting to connect to " << remote_addr_.ToString();

        // Connect
        if (::connect(raw_socket, reinterpret_cast<struct sockaddr*>(&remote_addr), sizeof(remote_addr)) < 0) {
            RTC_LOG(LS_ERROR) << "Failed to connect raw socket, errno: " << strerror(errno);
            ::close(raw_socket);
            return false;
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
        OnConnect(tcp_socket_.get());

        return true;
    };
    return network_thread()->BlockingCall(std::move(task));
}

void DirectCaller::OnConnect(rtc::AsyncPacketSocket* socket) {
    RTC_LOG(LS_INFO) << "Connected to " << remote_addr_.ToString();
    
    // Start the message sequence
    SendMessage("HELLO");
}

void DirectCaller::OnMessage(rtc::AsyncPacketSocket* socket,
                           const unsigned char* data,
                           size_t len,
                           const rtc::SocketAddress& remote_addr) {
    std::string message(reinterpret_cast<const char*>(data), len);
    RTC_LOG(LS_INFO) << "Caller received: " << message;

    if (message == "WELCOME") {
       SendMessage("INIT");
    } 
    else if (message == "WAITING") { // Changed from if to else if for clarity
        Start();
    } 
    else if (message == "OK") {
        ShutdownInternal();
        QuitThreads();
    } else {
        HandleMessage(socket, message, remote_addr);
    }
}

void DirectCaller::Disconnect() {
    RTC_LOG(LS_INFO) << "Caller signaling disconnect, sending CANCEL.";
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
    // // Call the base class Disconnect to close the socket and reset PeerConnection
    // DirectApplication::Disconnect(); // REMOVED - Let ShutdownInternal handle PC, external logic handles socket if needed after Wait
}