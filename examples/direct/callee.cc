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

#include "direct.h"

// Function to parse IP address and port from a string in the format "IP:PORT"
// From option.cc
bool ParseIpAndPort(const std::string& ip_port, std::string& ip, int& port);

// DirectCallee Implementation
DirectCallee::DirectCallee(Options opts) 
    : DirectPeer(opts)
{ 
    std::string ip; local_port_ = 0;
    ParseIpAndPort(opts.address, ip, local_port_); 
}

DirectCallee::~DirectCallee() {
    if (tcp_socket_) {
        tcp_socket_->Close();
    }
    if (listen_socket_) {
        listen_socket_.reset();
    }
    Cleanup();
}

bool DirectCallee::StartListening() {

    auto task = [this]() -> bool {
        // Create raw socket
        int raw_socket = ::socket(AF_INET, SOCK_STREAM, 0);
        if (raw_socket < 0) {
            RTC_LOG(LS_ERROR) << "Failed to create socket, errno: " << errno;
            return false;
        }

        // Setup server address
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(local_port_);
        addr.sin_addr.s_addr = INADDR_ANY;

        // Bind
        if (::bind(raw_socket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            RTC_LOG(LS_ERROR) << "Failed to bind, errno: " << errno;
            ::close(raw_socket);
            return false;
        }

        // Listen
        if (::listen(raw_socket, 5) < 0) {
            RTC_LOG(LS_ERROR) << "Failed to listen, errno: " << errno;
            ::close(raw_socket);
            return false;
        }

        // Wrap the listening socket (Restoring original logic)
        auto wrapped_socket = pss()->WrapSocket(raw_socket);
        if (!wrapped_socket) {
            RTC_LOG(LS_ERROR) << "Failed to wrap socket";
            ::close(raw_socket);
            return false;
        }

        listen_socket_ = std::make_unique<rtc::AsyncTcpListenSocket>(
            std::unique_ptr<rtc::Socket>(wrapped_socket));
        
        listen_socket_->SignalNewConnection.connect(this, &DirectCallee::OnNewConnection);

        RTC_LOG(LS_INFO) << "Server listening on port " << local_port_;
        return true;
    };
    return network_thread()->BlockingCall(std::move(task));
}

void DirectCallee::OnNewConnection(rtc::AsyncListenSocket* listen_socket, 
                                 rtc::AsyncPacketSocket* new_socket) {
    RTC_LOG(LS_INFO) << "New connection received";
    
    if (!new_socket) {
        RTC_LOG(LS_ERROR) << "New socket is null";
        return;
    }

    // Use a pointer to the specific socket for this connection
    rtc::AsyncTCPSocket* current_client_socket = static_cast<rtc::AsyncTCPSocket*>(new_socket);
    RTC_LOG(LS_INFO) << "Connection accepted from " << current_client_socket->GetRemoteAddress().ToString();

    // Overwrite the base class socket - assumes only one active connection for SendMessage
    // If supporting multiple clients, this needs rethinking.
    tcp_socket_.reset(current_client_socket);

    // Register callback on the specific socket for this connection
    current_client_socket->RegisterReceivedPacketCallback(
        // Capture the specific socket pointer for this callback instance
        [this, current_client_socket](rtc::AsyncPacketSocket* socket_param, const rtc::ReceivedPacket& packet) {
            // Verify the callback is running for the socket it was registered on
            if (socket_param != current_client_socket) {
                 RTC_LOG(LS_WARNING) << "Received packet on unexpected socket instance. Ignoring.";
                 return;
            }
            std::string received_data(reinterpret_cast<const char*>(packet.payload().data()), packet.payload().size());
            RTC_LOG(LS_INFO) << "[Callback Lambda] Received data: '" << received_data << "' on socket " << current_client_socket;
            // Pass the correct socket instance (current_client_socket) to OnMessage
            OnMessage(current_client_socket, 
                      reinterpret_cast<const unsigned char*>(packet.payload().data()), 
                      packet.payload().size(), 
                      packet.source_address());
        });
    
    RTC_LOG(LS_INFO) << "Callback registered for incoming messages on socket " << current_client_socket;
}

void DirectCallee::OnMessage(rtc::AsyncPacketSocket* socket,
                           const unsigned char* data,
                           size_t len,
                           const rtc::SocketAddress& remote_addr) {
    std::string message(reinterpret_cast<const char*>(data), len);
    RTC_LOG(LS_INFO) << "[Callee::OnMessage] Start - Received: '" << message << "' from " << remote_addr.ToString();

    if (message == "HELLO") {
        SendMessage("WELCOME");
    } else if (message == "BYE") {
        SendMessage("OK");
        Shutdown();
        QuitThreads();
    } else if (message == "CANCEL") {
        RTC_LOG(LS_INFO) << "Received CANCEL from " << remote_addr.ToString() << ". Resetting state for this client.";
        if (socket == tcp_socket_.get()) {
            // 1. Close the specific client socket
            tcp_socket_->Close();
            tcp_socket_.reset();

            // 2. Reset PeerConnection state (mimic part of base Disconnect)
            if (peer_connection_) {
                RTC_LOG(LS_INFO) << "Closing PeerConnection due to CANCEL";
                peer_connection_->Close();
                peer_connection_ = nullptr;
            }
            // 3. Reset message sequence counters (mimic part of base Disconnect)
            ice_candidates_sent_ = 0;
            ice_candidates_received_ = 0;
            sdp_fragments_sent_ = 0;
            sdp_fragments_received_ = 0;

            // 4. DO NOT call base Disconnect() - keep listener alive
            // 5. DO NOT call StartListening() - listener is still alive
            
            RTC_LOG(LS_INFO) << "Callee state reset. Ready for new connection.";

        } else {
            RTC_LOG(LS_WARNING) << "Received CANCEL on an unexpected or outdated socket pointer";
        }
        // // RTC_LOG(LS_INFO) << "Continuing to listen for new connections after CANCEL."; // Remove this log
    } else {
        RTC_LOG(LS_INFO) << "[Callee::OnMessage] Falling through to Peer::HandleMessage for message: '" << message << "'";
        HandleMessage(socket, message, remote_addr);
    }
}
