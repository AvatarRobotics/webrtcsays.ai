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

#include <arpa/inet.h>
#include <errno.h>  // For errno
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>  // For close

#include <chrono>
#include <cstdlib>
#include <cstring>  // For memset
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "direct.h"

#if TARGET_OS_IOS || TARGET_OS_OSX
#include "bonjour.h"
#endif // #if TARGET_OS_IOS || TARGET_OS_OSX

// DirectCallee Implementation
DirectCallee::DirectCallee(Options opts) : DirectPeer(opts) {
  std::string ip;
  local_port_ = 0;
  ParseIpAndPort(opts.address, ip, local_port_);
}

DirectCallee::~DirectCallee() {
  if (tcp_socket_) {
    tcp_socket_->Close();
  }
  if (listen_socket_) {
    listen_socket_.reset();
  }
}

bool DirectCallee::StartListening() {
  // Core listening setup logic as a callable
  auto listen_fn = [this]() -> bool {
    const int kMaxRetries = 10;
    const int kMaxRetryTimeMs = 30000; // 30 seconds total
    const int kInitialDelayMs = 100;   // Start with 100ms delay
    
    auto start_time = std::chrono::steady_clock::now();
    
    for (int attempt = 0; attempt < kMaxRetries; ++attempt) {
      // Check if we've exceeded the total timeout
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - start_time).count();
      if (elapsed >= kMaxRetryTimeMs) {
        RTC_LOG(LS_ERROR) << "Binding timeout exceeded (" << kMaxRetryTimeMs << "ms). Giving up.";
        break;
      }
      
      // Create raw socket
      int raw_socket = ::socket(AF_INET, SOCK_STREAM, 0);
      if (raw_socket < 0) {
        RTC_LOG(LS_ERROR) << "Failed to create socket, errno: "
                          << strerror(errno);
        return false;
      }

      // Set SO_REUSEADDR to allow address reuse
      int reuse = 1;
      if (::setsockopt(raw_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        RTC_LOG(LS_WARNING) << "Failed to set SO_REUSEADDR, errno: " << strerror(errno);
        // Continue anyway, this is not fatal
      }

      // Setup server address
      struct sockaddr_in addr;
      memset(&addr, 0, sizeof(addr));
      addr.sin_family = AF_INET;
      addr.sin_port = htons(local_port_);
      addr.sin_addr.s_addr = INADDR_ANY;

      // Attempt to bind
      if (::bind(raw_socket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        int bind_errno = errno;
        RTC_LOG(LS_WARNING) << "Bind attempt " << (attempt + 1) << "/" << kMaxRetries 
                           << " failed, errno: " << strerror(bind_errno) 
                           << " (port " << local_port_ << ")";
        ::close(raw_socket);
        
        // If not "Address already in use", don't retry
        if (bind_errno != EADDRINUSE) {
          RTC_LOG(LS_ERROR) << "Non-recoverable bind error: " << strerror(bind_errno);
          return false;
        }
        
        // Calculate delay with exponential backoff (100ms, 200ms, 400ms, 800ms, 1600ms, then cap at 2000ms)
        int delay_ms = kInitialDelayMs * (1 << std::min(attempt, 4));
        delay_ms = std::min(delay_ms, 2000); // Cap at 2 seconds
        
        // Make sure we don't exceed total timeout
        if (elapsed + delay_ms >= kMaxRetryTimeMs) {
          delay_ms = kMaxRetryTimeMs - elapsed;
          if (delay_ms <= 0) {
            RTC_LOG(LS_ERROR) << "No time left for retry delay. Giving up.";
            break;
          }
        }
        
        RTC_LOG(LS_INFO) << "Retrying bind in " << delay_ms << "ms...";
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        continue;
      }
      
      // Bind succeeded, continue with socket setup
      RTC_LOG(LS_INFO) << "Bind succeeded on attempt " << (attempt + 1) << " (port " << local_port_ << ")";

      // Retrieve the actual port assigned by the OS if port was 0
      socklen_t addrlen = sizeof(addr);
      if (getsockname(raw_socket, (struct sockaddr*)&addr, &addrlen) == 0) {
          local_port_ = ntohs(addr.sin_port);
      }

      // Listen
      if (::listen(raw_socket, 5) < 0) {
        RTC_LOG(LS_ERROR) << "Failed to listen, errno: " << strerror(errno);
        ::close(raw_socket);
        return false;
      }

      // Wrap the listening socket
      auto wrapped_socket = pss()->WrapSocket(raw_socket);
      if (!wrapped_socket) {
        RTC_LOG(LS_ERROR) << "Failed to wrap socket";
        ::close(raw_socket);
        return false;
      }

      listen_socket_ = std::make_unique<rtc::AsyncTcpListenSocket>(
          std::unique_ptr<rtc::Socket>(wrapped_socket));
      listen_socket_->SignalNewConnection.connect(this,
                                                  &DirectCallee::OnNewConnection);

      RTC_LOG(LS_INFO) << "Server listening on port " << local_port_;

      #if TARGET_OS_IOS || TARGET_OS_OSX
      // Bonjour advertisement
      if (opts_.bonjour && !opts_.bonjour_name.empty()) {
          if (AdvertiseBonjourService(opts_.bonjour_name, local_port_)) {
              RTC_LOG(LS_INFO) << "Bonjour advertised as '" << opts_.bonjour_name << "' on port " << local_port_;
          } else {
              RTC_LOG(LS_WARNING) << "Bonjour advertisement failed for '" << opts_.bonjour_name << "'";
          }
      }
      #endif // #if TARGET_OS_IOS || TARGET_OS_OSX
      return true;
    } // End of retry loop
    
    // If we get here, all retry attempts failed
    RTC_LOG(LS_ERROR) << "Failed to bind after " << kMaxRetries << " attempts within " 
                      << kMaxRetryTimeMs << "ms timeout. Giving up.";
    return false;
  };
  // If already on network thread, run directly to avoid BlockingCall DCHECK
  if (network_thread()->IsCurrent()) {
    return listen_fn();
  }
  network_thread()->Restart();
  return network_thread()->BlockingCall(std::move(listen_fn));
}

void DirectCallee::OnNewConnection(rtc::AsyncListenSocket* listen_socket,
                                   rtc::AsyncPacketSocket* new_socket) {
  RTC_LOG(LS_INFO) << "New connection received";

  if (!new_socket) {
    RTC_LOG(LS_ERROR) << "New socket is null";
    return;
  }

  // Use a pointer to the specific socket for this connection
  rtc::AsyncTCPSocket* current_client_socket =
      static_cast<rtc::AsyncTCPSocket*>(new_socket);
  RTC_LOG(LS_INFO) << "Connection accepted from "
                   << current_client_socket->GetRemoteAddress().ToString()
                   << " on socket (" << new_socket << ")";

  // Overwrite the base class socket - assumes only one active connection for
  // SendMessage If supporting multiple clients, this needs rethinking.
  tcp_socket_.reset(current_client_socket);

  // Register callback on the specific socket for this connection
  current_client_socket->RegisterReceivedPacketCallback(
      // Capture the specific socket pointer for this callback instance
      [this, current_client_socket](rtc::AsyncPacketSocket* socket_param,
                                    const rtc::ReceivedPacket& packet) {
        // Verify the callback is running for the socket it was registered on
        if (socket_param != current_client_socket) {
          RTC_LOG(LS_WARNING)
              << "Received packet on unexpected socket instance. Ignoring.";
          return;
        }
        RTC_LOG(LS_INFO) << "Received packet of size: "
                         << packet.payload().size() << " on socket "
                         << current_client_socket;
        // Pass the correct socket instance (current_client_socket) to OnMessage
        OnMessage(
            current_client_socket,
            reinterpret_cast<const unsigned char*>(packet.payload().data()),
            packet.payload().size(), packet.source_address());
      });
  current_client_socket->SubscribeCloseEvent(
      this,  // use `this` or any stable tag
      [this](rtc::AsyncPacketSocket* socket, int err) {
        RTC_LOG(LS_INFO) << "Wire‐level socket closed, err=" << err;
        // wake up your main thread or signal your Event here
        connection_closed_event_.Set();
      });

  RTC_LOG(LS_INFO) << "Callback registered for incoming messages on socket "
                   << current_client_socket;
}

void DirectCallee::OnMessage(rtc::AsyncPacketSocket* socket,
                             const unsigned char* data,
                             size_t len,
                             const rtc::SocketAddress& remote_addr) {
  // Quick log of incoming message (up to 127 chars)
  char bufLog[128];
  size_t cnt = len < sizeof(bufLog) - 1 ? len : sizeof(bufLog) - 1;
  memcpy(bufLog, data, cnt);
  bufLog[cnt] = '\0';
  RTC_LOG(LS_INFO) << "Callee received: " << bufLog << " from "
                   << remote_addr.ToString();

  // Match known commands via length and memcmp
  if (len == 5 && memcmp(data, "HELLO", 5) == 0) {
    SendMessage("WELCOME");
  } else if (len == 3 && memcmp(data, "BYE", 3) == 0) {
    SendMessage("OK");
    // Skipping synchronous shutdown for BYE to avoid blocking
  } else if (len == 6 && memcmp(data, "CANCEL", 6) == 0) {
    RTC_LOG(LS_INFO) << "Received CANCEL from " << remote_addr.ToString()
                     << ". Restarting listener.";
    OnCancel(socket);
  } else {
    // Forward other messages to default handler
    std::string message;
    message.resize(len);
    memcpy(&message[0], data, len);
    HandleMessage(socket, message, remote_addr);
  }
}

void DirectCallee::OnCancel(rtc::AsyncPacketSocket* socket) {
  RTC_LOG(LS_INFO) << "Callee socket closed";
  if (socket == tcp_socket_.get()) {
    // Full teardown and re-init on main thread
    main_thread()->PostTask([this]() {
      RTC_LOG(LS_INFO) << "Full teardown initiated via CANCEL";
      // 1) Cleanup completely (threads, sockets, factories)
      Cleanup();
      // 2) Re-initialize threads and WebRTC infrastructure
      if (!Initialize()) {
        RTC_LOG(LS_ERROR) << "Re-initialize failed after CANCEL";
        return;
      }
      // 3) Restart listening socket
      if (!StartListening()) {
        RTC_LOG(LS_ERROR) << "StartListening failed after CANCEL";
        return;
      }
      // 4) Resume background processing
      RunOnBackgroundThread();
      RTC_LOG(LS_INFO) << "Callee fully restarted after CANCEL.";
    });
  } else {
    RTC_LOG(LS_WARNING) << "Received CANCEL on an unexpected socket.";
  }
}

void DirectCallee::OnClose(rtc::AsyncPacketSocket* socket) {
  RTC_LOG(LS_INFO) << "Callee socket closed";
}
