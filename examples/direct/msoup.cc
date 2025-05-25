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

#include "msoup.h"
#include "direct.h"

std::string MediaSoupWrapper::generate_websocket_key() {
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<> dis(0, 255);
  std::string key(16, '\0');
  for (int i = 0; i < 16; ++i) {
    key[i] = static_cast<char>(dis(gen));
  }
  return base64_encode(key);
}

std::string MediaSoupWrapper::generate_peer_id() {
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<> dis(0, 35);
  const std::string chars = "0123456789abcdefghijklmnopqrstuvwxyz";
  std::string peer_id(8, '\0');
  for (int i = 0; i < 8; ++i) {
    peer_id[i] = chars[dis(gen)];
  }
  return peer_id;
}

const absl::string_view base64_chars =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string MediaSoupWrapper::base64_encode(const std::string& in) {
  std::string out;
  int val = 0, valb = -6;
  for (unsigned char c : in) {
    val = (val << 8) + c;
    valb += 8;
    while (valb >= 0) {
      out.push_back(base64_chars[(val >> valb) & 0x3F]);
      valb -= 6;
    }
  }
  if (valb > -6) {
    out.push_back(base64_chars[((val << 8) >> (valb + 8)) & 0x3F]);
  }
  while (out.size() % 4) {
    out.push_back('=');
  }
  return out;
}

void MediaSoupWrapper::send_ping() {
  std::vector<unsigned char> frame;
  frame.push_back(0x89);  // FIN + Ping frame
  frame.push_back(0x00);  // Unmasked, length 0 (no payload)

  std::lock_guard<std::mutex> lock(ssl_mutex);
  int bytes_written;
  if (use_ssl_) {
    if (!ssl) {
      APP_LOG(AS_ERROR) << "SSL object is null in send_ping";
      return;
    }
    bytes_written = SSL_write(ssl, frame.data(), frame.size());
    if (bytes_written <= 0) {
      APP_LOG(AS_ERROR) << "SSL_write failed in send_ping: "
                        << ERR_error_string(ERR_get_error(), nullptr);
      return;
    }
  } else {
    bytes_written = send(sockfd, frame.data(), frame.size(), 0);
    if (bytes_written <= 0) {
      APP_LOG(AS_ERROR) << "send failed in send_ping: " << strerror(errno);
      return;
    }
  }
  APP_LOG(AS_INFO) << "Sent ping frame";
}

void MediaSoupWrapper::send_pong_frame(const std::vector<unsigned char>& ping_payload) {
  std::vector<unsigned char> frame;
  frame.push_back(0x8A);  // FIN + Pong frame

  size_t length = ping_payload.size();
  if (length <= 125) {
    frame.push_back(0x00 | length);  // Unmasked + length
  } else if (length <= 65535) {
    frame.push_back(0x00 | 126);
    frame.push_back((length >> 8) & 0xFF);
    frame.push_back(length & 0xFF);
  } else {
    frame.push_back(0x00 | 127);
    frame.push_back(0);
    frame.push_back(0);
    frame.push_back(0);
    frame.push_back(0);
    frame.push_back((length >> 24) & 0xFF);
    frame.push_back((length >> 16) & 0xFF);
    frame.push_back((length >> 8) & 0xFF);
    frame.push_back(length & 0xFF);
  }

  frame.insert(frame.end(), ping_payload.begin(), ping_payload.end());

  std::lock_guard<std::mutex> lock(ssl_mutex);
  int bytes_written;
  if (use_ssl_) {
    if (!ssl) {
      APP_LOG(AS_ERROR) << "SSL object is null in send_pong_frame";
      return;
    }
    bytes_written = SSL_write(ssl, frame.data(), frame.size());
    if (bytes_written <= 0) {
      APP_LOG(AS_ERROR) << "SSL_write failed in send_pong_frame: "
                        << ERR_error_string(ERR_get_error(), nullptr);
      return;
    }
  } else {
    bytes_written = send(sockfd, frame.data(), frame.size(), 0);
    if (bytes_written <= 0) {
      APP_LOG(AS_ERROR) << "send failed in send_pong_frame: "
                        << strerror(errno);
      return;
    }
  }
  APP_LOG(AS_INFO) << "Sent pong frame in response to ping";
}

MediaSoupWrapper::MediaSoupWrapper(const std::string& cert_file, bool use_ssl)
    : sockfd(-1),
      ctx(nullptr),
      ssl(nullptr),
      use_ssl_(use_ssl),
      running_(false),
      cert_file_path_(cert_file) {
  // Initialize OpenSSL
  SSL_library_init();
  OpenSSL_add_all_algorithms();
  SSL_load_error_strings();
  ERR_load_crypto_strings();

  // Create SSL context with proper method and options
  ctx = SSL_CTX_new(TLS_client_method());
  if (!ctx) {
    APP_LOG(AS_ERROR) << "Failed to create SSL context: "
                      << ERR_error_string(ERR_get_error(), nullptr);
    return;
  }

  // Set verification mode to NONE - accept self-signed certificates
  SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);

  // Set modern cipher list
  if (SSL_CTX_set_cipher_list(ctx, "HIGH:!aNULL:!kRSA:!PSK:!SRP:!MD5:!RC4") !=
      1) {
    APP_LOG(AS_ERROR) << "Failed to set cipher list: "
                      << ERR_error_string(ERR_get_error(), nullptr);
  }

  // Enable TLS extensions and SNI
  SSL_CTX_set_options(ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_TLSv1 |
                               SSL_OP_NO_TLSv1_1);
}

void MediaSoupWrapper::set_key_file(const std::string& key_file) {
  key_file_path_ = key_file;
}

void MediaSoupWrapper::set_network_thread(rtc::Thread* thread) {
  network_thread_ = thread;
}

MediaSoupWrapper::~MediaSoupWrapper() {
  stop_listening();
  if (ssl) {
    SSL_free(ssl);
    ssl = nullptr;
  }
  if (ctx) {
    SSL_CTX_free(ctx);
    ctx = nullptr;
  }
  if (sockfd != -1) {
    ::close(sockfd);
    sockfd = -1;
  }
  // Clean up OpenSSL
  ERR_free_strings();
  EVP_cleanup();
}

bool MediaSoupWrapper::connect(const std::string& host, const std::string& port) {
  APP_LOG(AS_INFO) << "Connecting to WebSocket server at " << host << ":"
                   << port;

  // DNS resolution
  struct addrinfo hints, *res;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;

  int status = getaddrinfo(host.c_str(), port.c_str(), &hints, &res);
  if (status != 0) {
    APP_LOG(AS_INFO) << "getaddrinfo failed: " << gai_strerror(status);
    return false;
  }

  // Create socket
  sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (sockfd < 0) {
    APP_LOG(AS_ERROR) << "Socket creation failed: " << strerror(errno);
    freeaddrinfo(res);
    return false;
  }
  APP_LOG(AS_INFO) << "Socket created: " << sockfd;

  // Connect to server
  if (::connect(sockfd, res->ai_addr, res->ai_addrlen) < 0) {
    APP_LOG(AS_ERROR) << "Socket connect failed: " << strerror(errno);
    close(sockfd);
    freeaddrinfo(res);
    return false;
  }
  APP_LOG(AS_INFO) << "TCP connection established to " << host << ":" << port;

  // No SSL for local proxy
  use_ssl_ = (target_port_ == "443" || target_port_ == "4443");

  if (use_ssl_) {
    APP_LOG(AS_INFO) << "Setting up SSL for direct connection";

    // Check if context is valid
    if (!ctx) {
      APP_LOG(AS_ERROR) << "SSL context is null, cannot create SSL connection";
      close(sockfd);
      freeaddrinfo(res);
      return false;
    }

    // Load client certificate if provided
    if (!cert_file_path_.empty()) {
      APP_LOG(AS_INFO) << "Loading client certificate from: "
                       << cert_file_path_;
      if (SSL_CTX_use_certificate_file(ctx, cert_file_path_.c_str(),
                                       SSL_FILETYPE_PEM) <= 0) {
        APP_LOG(AS_ERROR) << "Failed to load certificate file: "
                          << ERR_error_string(ERR_get_error(), nullptr);
        // Continue anyway, certificate might not be required
      }
    }

    // Load private key if provided
    if (!key_file_path_.empty()) {
      APP_LOG(AS_INFO) << "Loading private key from: " << key_file_path_;
      if (SSL_CTX_use_PrivateKey_file(ctx, key_file_path_.c_str(),
                                      SSL_FILETYPE_PEM) <= 0) {
        APP_LOG(AS_ERROR) << "Failed to load private key file: "
                          << ERR_error_string(ERR_get_error(), nullptr);
        // Continue anyway, key might not be required
      }

      // Check key and certificate compatibility
      if (SSL_CTX_check_private_key(ctx) <= 0) {
        APP_LOG(AS_ERROR) << "Private key does not match the certificate: "
                          << ERR_error_string(ERR_get_error(), nullptr);
        // Continue anyway, might still work
      }
    }

    // Create new SSL object
    ssl = SSL_new(ctx);
    if (!ssl) {
      APP_LOG(AS_ERROR) << "Failed to create SSL object: "
                        << ERR_error_string(ERR_get_error(), nullptr);
      close(sockfd);
      freeaddrinfo(res);
      return false;
    }

    // Set socket to SSL object
    if (SSL_set_fd(ssl, sockfd) != 1) {
      APP_LOG(AS_ERROR) << "Failed to set SSL file descriptor: "
                        << ERR_error_string(ERR_get_error(), nullptr);
      SSL_free(ssl);
      ssl = nullptr;
      close(sockfd);
      freeaddrinfo(res);
      return false;
    }

    // Set SNI hostname
    if (SSL_set_tlsext_host_name(ssl, target_host_.c_str()) != 1) {
      APP_LOG(AS_ERROR) << "Failed to set SNI hostname: "
                        << ERR_error_string(ERR_get_error(), nullptr);
      // Continue anyway, SNI is not critical
    }

    // Perform SSL handshake
    int connect_result = SSL_connect(ssl);
    if (connect_result <= 0) {
      int ssl_err = SSL_get_error(ssl, connect_result);
      APP_LOG(AS_ERROR) << "SSL connection failed with error " << ssl_err
                        << ": " << ERR_error_string(ERR_get_error(), nullptr);
      SSL_free(ssl);
      ssl = nullptr;
      close(sockfd);
      freeaddrinfo(res);
      return false;
    }

    // Log successful SSL connection
    APP_LOG(AS_INFO) << "SSL connection established with "
                     << SSL_get_cipher(ssl)
                     << ", protocol: " << SSL_get_version(ssl);

    // Verify certificate (optional, we're using SSL_VERIFY_NONE)
    X509* cert = SSL_get_peer_certificate(ssl);
    if (cert) {
      APP_LOG(AS_INFO) << "Server certificate verified";
      X509_free(cert);
    } else {
      APP_LOG(AS_WARNING) << "No server certificate received";
    }
  }

  freeaddrinfo(res);
  return true;
}

bool MediaSoupWrapper::send_http_request(const std::string& path) {
  std::string full_path = "/?roomId=" + room_id_ + "&peerId=" + peer_id_;
  std::string request =
      "GET " + full_path +
      " HTTP/1.1\r\n"
      "Host: " +
      target_host_ + +":" + target_port_ + "\r\n" +
      "Connection: Upgrade\r\n"
      "Pragma: no-cache\r\n"
      "Cache-Control: no-cache\r\n"
      "User-Agent: Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
      "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/132.0.0.0 "
      "Safari/537.36\r\n"
      "Upgrade: websocket\r\n"
      "Origin: " +
      "https://v3demo.mediasoup.org\r\n"
      "Sec-WebSocket-Version: 13\r\n"
      "Accept-Encoding: gzip, deflate, br, zstd\r\n"
      "Accept-Language: "
      "en-US,en;q=0.9,ru;q=0.8,zh-CN;q=0.7,zh;q=0.6,ky;q=0.5,bg;q=0.4\r\n"
      "Sec-WebSocket-Key: " +
      generate_websocket_key() +
      "\r\n"
      "Sec-WebSocket-Extensions: permessage-deflate; "
      "client_max_window_bits\r\n"
      "Sec-WebSocket-Protocol: protoo\r\n"
      "\r\n";
  APP_LOG(AS_INFO) << "Sending HTTP request: " << request;

  int bytes_written;
  if (use_ssl_) {
    bytes_written = SSL_write(ssl, request.c_str(), request.length());
    if (bytes_written <= 0) {
      APP_LOG(AS_ERROR) << "SSL_write failed: "
                        << ERR_error_string(ERR_get_error(), nullptr);
      return false;
    }
  } else {
    bytes_written = send(sockfd, request.c_str(), request.length(), 0);
    if (bytes_written <= 0) {
      APP_LOG(AS_ERROR) << "send failed: " << strerror(errno);
      return false;
    }
  }
  APP_LOG(AS_INFO) << "Sent " << bytes_written << " bytes";

  char buffer[4096];
  std::string response;
  int total_bytes_read = 0;
  int timeout_ms = 10000;
  int elapsed_ms = 0;
  const int poll_interval_ms = 100;

  while (elapsed_ms < timeout_ms) {
    int bytes_read;
    if (use_ssl_) {
      bytes_read = SSL_read(ssl, buffer + total_bytes_read,
                            sizeof(buffer) - total_bytes_read - 1);
      if (bytes_read <= 0) {
        int err = SSL_get_error(ssl, bytes_read);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
          std::this_thread::sleep_for(
              std::chrono::milliseconds(poll_interval_ms));
          elapsed_ms += poll_interval_ms;
          continue;
        }
        APP_LOG(AS_ERROR) << "SSL_read failed: "
                          << ERR_error_string(ERR_get_error(), nullptr);
        return false;
      }
    } else {
      bytes_read = recv(sockfd, buffer + total_bytes_read,
                        sizeof(buffer) - total_bytes_read - 1, 0);
      if (bytes_read < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          std::this_thread::sleep_for(
              std::chrono::milliseconds(poll_interval_ms));
          elapsed_ms += poll_interval_ms;
          continue;
        }
        APP_LOG(AS_ERROR) << "recv failed: " << strerror(errno);
        return false;
      } else if (bytes_read == 0) {
        APP_LOG(AS_ERROR) << "Connection closed by server during handshake";
        return false;
      }
    }

    total_bytes_read += bytes_read;
    buffer[total_bytes_read] = '\0';
    response = std::string(buffer, total_bytes_read);
    APP_LOG(AS_INFO) << "Received handshake data: " << response;
    if (response.find("\r\n\r\n") != std::string::npos) {
      APP_LOG(AS_INFO) << "Full handshake response: " << response;
      if (response.find("101 Switching Protocols") != std::string::npos) {
        return true;
      } else {
        APP_LOG(AS_ERROR) << "WebSocket handshake failed: " << response;
        return false;
      }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(poll_interval_ms));
    elapsed_ms += poll_interval_ms;
  }

  APP_LOG(AS_ERROR) << "Handshake timed out after " << timeout_ms
                    << "ms, received: " << response;
  return false;
}

bool MediaSoupWrapper::send_websocket_frame(const std::string& message) {
  APP_LOG(AS_VERBOSE) << "Sending WebSocket message: " << message;

  std::vector<unsigned char> frame;
  frame.push_back(0x81);  // FIN + text frame

  size_t length = message.length();
  if (length <= 125) {
    frame.push_back(0x80 | length);  // Masked + length
  } else if (length <= 65535) {
    frame.push_back(0x80 | 126);
    frame.push_back((length >> 8) & 0xFF);
    frame.push_back(length & 0xFF);
  } else {
    frame.push_back(0x80 | 127);
    frame.push_back(0);
    frame.push_back(0);
    frame.push_back(0);
    frame.push_back(0);
    frame.push_back((length >> 24) & 0xFF);
    frame.push_back((length >> 16) & 0xFF);
    frame.push_back((length >> 8) & 0xFF);
    frame.push_back(length & 0xFF);
  }

  // Generate random mask key
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<> dis(0, 255);
  std::vector<unsigned char> mask(4);
  for (int i = 0; i < 4; ++i) {
    mask[i] = dis(gen);
    frame.push_back(mask[i]);
  }

  // Log mask key for debugging
  std::stringstream mask_ss;
  mask_ss << "Mask key: ";
  for (int i = 0; i < 4; ++i) {
    mask_ss << std::hex << std::setw(2) << std::setfill('0') << (int)mask[i]
            << " ";
  }
  APP_LOG(AS_VERBOSE) << mask_ss.str();

  // Apply mask to payload
  std::vector<unsigned char> masked_payload(length);
  for (size_t i = 0; i < length; ++i) {
    masked_payload[i] = message[i] ^ mask[i % 4];
  }

  // Add masked payload to frame
  frame.insert(frame.end(), masked_payload.begin(), masked_payload.end());

  // Log frame details for debugging
  std::stringstream header_dump;
  header_dump << "WebSocket frame header: ";
  for (size_t i = 0; i < std::min(frame.size() - length, size_t(16)); ++i) {
    header_dump << std::hex << std::setw(2) << std::setfill('0')
                << static_cast<int>(frame[i]) << " ";
  }
  APP_LOG(AS_VERBOSE) << header_dump.str();
  APP_LOG(AS_VERBOSE) << "Frame total size: " << frame.size()
                      << " bytes (header: " << (frame.size() - length)
                      << " bytes, payload: " << length << " bytes)";

  std::lock_guard<std::mutex> lock(ssl_mutex);
  if (sockfd == -1) {
    APP_LOG(AS_ERROR) << "Cannot send frame: socket is invalid (closed)";
    running_ = false;
    return false;
  }

  int bytes_written;
  if (use_ssl_) {
    if (!ssl) {
      APP_LOG(AS_ERROR) << "SSL object is null";
      return false;
    }
    bytes_written = SSL_write(ssl, frame.data(), frame.size());
    if (bytes_written <= 0) {
      int ssl_err = SSL_get_error(ssl, bytes_written);
      APP_LOG(AS_ERROR) << "SSL_write failed: "
                        << ERR_error_string(ssl_err, nullptr)
                        << ", error code: " << ssl_err;
      return false;
    }
  } else {
    bytes_written = send(sockfd, frame.data(), frame.size(), 0);
    if (bytes_written <= 0) {
      APP_LOG(AS_ERROR) << "send failed: " << strerror(errno)
                        << " (errno: " << errno << ")";
      if (errno == EPIPE || errno == ECONNRESET || errno == EBADF) {
        APP_LOG(AS_INFO)
            << "Connection broken or invalid, marking as disconnected";
        running_ = false;
      }
      return false;
    }
  }

  APP_LOG(AS_VERBOSE) << "Sent WebSocket frame successfully, bytes written: "
                      << bytes_written;

  // Force an immediate read to get the response
  if (network_thread_) {
    network_thread_->PostTask([this]() {
      if (running_.load() && !read_in_progress_.load()) {
        APP_LOG(AS_VERBOSE) << "Scheduling immediate read after sending frame";
        async_read();
      }
    });
  }

  return true;
}

std::vector<std::string> MediaSoupWrapper::process_websocket_frames() {
  std::lock_guard<std::mutex> lock(buffer_mutex_);
  std::vector<std::string> messages;
  std::string accumulated_payload;
  bool is_continuation = false;

  if (buffer_.empty()) {
    APP_LOG(AS_VERBOSE) << "process_websocket_frames: Buffer empty";
    return messages;
  }

  APP_LOG(AS_VERBOSE) << "process_websocket_frames: Buffer size: "
                      << buffer_.size();
  std::stringstream hex_dump;
  hex_dump << "Buffer hex (first 100 bytes): ";
  for (size_t i = 0; i < std::min(buffer_.size(), size_t(100)); i++) {
    hex_dump << std::hex << std::setw(2) << std::setfill('0')
             << (int)(unsigned char)buffer_[i] << " ";
  }
  APP_LOG(AS_VERBOSE) << hex_dump.str();

  size_t pos = 0;
  while (pos + 1 < buffer_.size()) {
    uint8_t first_byte = buffer_[pos];
    bool fin = (first_byte & 0x80) != 0;
    uint8_t opcode = first_byte & 0x0F;

    if (opcode > 0xA || (opcode > 0x2 && opcode < 0x8)) {
      std::stringstream ss;
      ss << "Invalid opcode " << std::hex << (int)opcode << " at pos "
         << std::dec << pos;
      APP_LOG(AS_ERROR) << ss.str();
      pos++;
      continue;
    }

    uint8_t second_byte = buffer_[pos + 1];
    bool masked = (second_byte & 0x80) != 0;
    uint64_t length = second_byte & 0x7F;
    size_t header_size = 2;

    APP_LOG(AS_VERBOSE) << "Parsing length at pos " << pos
                        << ": initial length=" << length;

    if (length == 126) {
      if (pos + 4 > buffer_.size()) {
        APP_LOG(AS_INFO) << "Incomplete 16-bit length field at pos " << pos;
        break;
      }
      length = (static_cast<uint64_t>(buffer_[pos + 2] & 0xFF) << 8) |
               static_cast<uint64_t>(buffer_[pos + 3] & 0xFF);
      header_size = 4;
      std::stringstream ss;
      ss << "16-bit length read: " << std::dec << length
         << " (bytes: " << std::hex << (int)(buffer_[pos + 2] & 0xFF) << " "
         << (int)(buffer_[pos + 3] & 0xFF) << ")";
      APP_LOG(AS_INFO) << ss.str();
    } else if (length == 127) {
      if (pos + 10 > buffer_.size()) {
        APP_LOG(AS_INFO) << "Incomplete 64-bit length field at pos " << pos;
        break;
      }
      length = (static_cast<uint64_t>(buffer_[pos + 2] & 0xFF) << 56) |
               (static_cast<uint64_t>(buffer_[pos + 3] & 0xFF) << 48) |
               (static_cast<uint64_t>(buffer_[pos + 4] & 0xFF) << 40) |
               (static_cast<uint64_t>(buffer_[pos + 5] & 0xFF) << 32) |
               (static_cast<uint64_t>(buffer_[pos + 6] & 0xFF) << 24) |
               (static_cast<uint64_t>(buffer_[pos + 7] & 0xFF) << 16) |
               (static_cast<uint64_t>(buffer_[pos + 8] & 0xFF) << 8) |
               static_cast<uint64_t>(buffer_[pos + 9] & 0xFF);
      header_size = 10;
      APP_LOG(AS_INFO) << "64-bit length read: " << length;
    }

    size_t mask_offset = masked ? 4 : 0;
    if (pos + header_size + mask_offset > buffer_.size()) {
      APP_LOG(AS_INFO) << "Incomplete header or mask at pos " << pos;
      break;
    }

    uint64_t total_frame_size = header_size + mask_offset + length;
    APP_LOG(AS_VERBOSE) << "Calculated total_frame_size: " << total_frame_size
                        << " (header_size=" << header_size
                        << ", mask_offset=" << mask_offset
                        << ", length=" << length << ")";

    if (pos + total_frame_size > buffer_.size()) {
      APP_LOG(AS_VERBOSE) << "Incomplete frame at pos " << pos << ", need "
                          << (pos + total_frame_size - buffer_.size())
                          << " more bytes"
                          << " (pos=" << pos
                          << ", total_frame_size=" << total_frame_size
                          << ", buffer_.size()=" << buffer_.size() << ")";
      break;
    }

    std::stringstream frame_ss;
    frame_ss << "Frame at pos " << std::dec << pos << ": opcode=" << std::hex
             << (int)opcode << ", length=" << std::dec << length
             << ", masked=" << masked << ", fin=" << fin
             << ", header size=" << header_size;
    APP_LOG(AS_VERBOSE) << frame_ss.str();

    if (opcode == 0x01 || opcode == 0x00) {
      std::string payload(buffer_.begin() + pos + header_size + mask_offset,
                          buffer_.begin() + pos + total_frame_size);
      if (masked) {
        unsigned char mask_key[4];
        for (int i = 0; i < 4; i++)
          mask_key[i] = buffer_[pos + header_size + i];
        for (size_t i = 0; i < payload.length(); i++) {
          payload[i] ^= mask_key[i % 4];
        }
      }

      if (opcode == 0x00) {
        if (!is_continuation) {
          APP_LOG(AS_ERROR) << "Continuation frame without start, ignoring";
          pos += total_frame_size;
          continue;
        }
        accumulated_payload += payload;
      } else if (is_continuation && fin) {
        accumulated_payload += payload;
        APP_LOG(AS_INFO) << "Extracted complete fragmented text payload: "
                         << accumulated_payload.substr(0, 100)
                         << (accumulated_payload.length() > 100 ? "..." : "");
        messages.push_back(accumulated_payload);
        accumulated_payload.clear();
        is_continuation = false;
      } else if (!is_continuation && fin) {
        APP_LOG(AS_VERBOSE)
            << "Extracted single text payload: " << payload.substr(0, 100)
            << (payload.length() > 100 ? "..." : "");
        messages.push_back(payload);
      } else {
        accumulated_payload = payload;
        is_continuation = true;
      }
    } else if (opcode == 0x09) {
      std::vector<unsigned char> ping_payload(
          buffer_.begin() + pos + header_size + mask_offset,
          buffer_.begin() + pos + total_frame_size);
      if (masked) {
        unsigned char mask_key[4];
        for (int i = 0; i < 4; i++)
          mask_key[i] = buffer_[pos + header_size + i];
        for (size_t i = 0; i < ping_payload.size(); i++) {
          ping_payload[i] ^= mask_key[i % 4];
        }
      }
      APP_LOG(AS_INFO) << "Received ping frame, length: " << length;
      send_pong_frame(ping_payload);
    } else if (opcode == 0x08) {
      APP_LOG(AS_INFO) << "Received close frame";
      running_ = false;
      messages.push_back("DISCONNECTED");
    } else {
      std::stringstream ss;
      ss << "Skipping unhandled opcode: " << std::hex << (int)opcode;
      APP_LOG(AS_WARNING) << ss.str();
    }

    pos += total_frame_size;
  }

  if (pos > 0) {
    APP_LOG(AS_VERBOSE) << "Erasing " << pos << " bytes from buffer";
    buffer_.erase(buffer_.begin(), buffer_.begin() + pos);
  }
  return messages;
}

void MediaSoupWrapper::async_read() {
  APP_LOG(AS_INFO) << "async_read: Starting, running_: " << running_.load()
                   << ", read_in_progress_: " << read_in_progress_.load()
                   << ", sockfd: " << sockfd
                   << ", thread: " << std::this_thread::get_id();
  if (!running_.load()) {
    APP_LOG(AS_ERROR) << "async_read: Not running, skipping";
    read_in_progress_ = false;
    return;
  }
  if (read_in_progress_.exchange(true)) {
    APP_LOG(AS_VERBOSE) << "async_read: Read already in progress, skipping";
    return;
  }
  if (!network_thread_) {
    APP_LOG(AS_ERROR) << "async_read: Network thread not set";
    read_in_progress_ = false;
    return;
  }
  if (sockfd == -1 || !is_connected()) {
    APP_LOG(AS_ERROR)
        << "async_read: Socket closed or invalid, attempting reconnect";
    running_ = false;
    read_in_progress_ = false;
    if (message_callback_)
      message_callback_("DISCONNECTED");
    network_thread_->PostTask([this]() { attempt_reconnect(); });
    return;
  }

  char buffer[8192];
  int total_bytes_read = 0;
  int attempts = 0;
  const int max_attempts = 5;  // Limit retries for partial reads
  bool no_more_data = false;

  do {
    int bytes_read = 0;
    if (use_ssl_) {
      if (!ssl) {
        APP_LOG(AS_ERROR) << "async_read: SSL object is null";
        running_ = false;
        read_in_progress_ = false;
        if (message_callback_)
          message_callback_("DISCONNECTED");
        return;
      }
      bytes_read = SSL_read(ssl, buffer + total_bytes_read,
                            sizeof(buffer) - total_bytes_read - 1);
      APP_LOG(AS_VERBOSE) << "async_read: SSL_read attempt " << attempts
                          << ", bytes_read: " << bytes_read;

      if (bytes_read <= 0) {
        int ssl_err = SSL_get_error(ssl, bytes_read);
        APP_LOG(AS_INFO) << "async_read: SSL_read returned " << bytes_read
                         << ", error: " << ssl_err;

        if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE) {
          APP_LOG(AS_INFO) << "async_read: SSL_ERROR_WANT_READ/WRITE, retrying";
          no_more_data = true;
          attempts++;
          if (attempts < max_attempts) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;  // Retry for more data
          }
          break;  // Exit loop if max retries reached
        } else if (ssl_err == SSL_ERROR_SYSCALL) {
          int saved_errno = errno;
          APP_LOG(AS_INFO) << "async_read: SSL syscall error, errno: "
                           << saved_errno << " - " << strerror(saved_errno);
          if (saved_errno == 0 && total_bytes_read > 0) {
            APP_LOG(AS_INFO) << "async_read: Partial data (" << total_bytes_read
                             << " bytes), retrying";
            attempts++;
            if (attempts < max_attempts) {
              std::this_thread::sleep_for(std::chrono::milliseconds(100));
              continue;  // Retry for partial data
            }
          }
          // Real error or no partial data, disconnect
          running_ = false;
          read_in_progress_ = false;
          if (message_callback_) {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            if (!buffer_.empty()) {
              APP_LOG(AS_INFO) << "Preserving " << buffer_.size()
                               << " bytes of partial data";
            }
            message_callback_("DISCONNECTED");
          }
          network_thread_->PostTask([this]() { attempt_reconnect(); });
          return;
        } else {
          APP_LOG(AS_ERROR) << "async_read: SSL error: "
                            << ERR_error_string(ERR_get_error(), nullptr);
          running_ = false;
          read_in_progress_ = false;
          if (message_callback_)
            message_callback_("DISCONNECTED");
          network_thread_->PostTask([this]() { attempt_reconnect(); });
          return;
        }
      }
    } else {
      bytes_read = recv(sockfd, buffer + total_bytes_read,
                        sizeof(buffer) - total_bytes_read - 1, 0);
      APP_LOG(AS_INFO) << "async_read: recv attempt " << attempts
                       << ", bytes_read: " << bytes_read;
      if (bytes_read < 0) {
        if (bytes_read < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
          APP_LOG(AS_INFO) << "async_read: EAGAIN/EWOULDBLOCK, retrying";
          no_more_data = true;
          attempts++;
          if (attempts < max_attempts) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
          }
          break;
        } else {
          APP_LOG(AS_ERROR)
              << "async_read: Connection lost: " << strerror(errno);
          running_ = false;
          read_in_progress_ = false;
          if (message_callback_)
            message_callback_("DISCONNECTED");
          network_thread_->PostTask([this]() { attempt_reconnect(); });
          return;
        }
      }
    }

    if (bytes_read > 0) {
      total_bytes_read += bytes_read;
      {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        buffer_.append(buffer + total_bytes_read - bytes_read, bytes_read);
        APP_LOG(AS_VERBOSE)
            << "async_read: Buffer size now: " << buffer_.size();
      }
      std::vector<std::string> messages = process_websocket_frames();
      for (const auto& message : messages) {
        if (message_callback_ && !message.empty()) {
          APP_LOG(AS_VERBOSE)
              << "async_read: Calling callback with: " << message.substr(0, 100)
              << (message.length() > 100 ? "..." : "");
          message_callback_(message);
        }
      }
      attempts = 0;  // Reset attempts on successful read
    }
  } while (running_.load() &&
           total_bytes_read < static_cast<int>(sizeof(buffer) - 1) &&
           attempts < max_attempts);

  if (attempts >= max_attempts) {
    APP_LOG(AS_ERROR) << "async_read: Max retry attempts (" << max_attempts
                      << ") reached";
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    if (!buffer_.empty()) {
      APP_LOG(AS_INFO) << "Preserving " << buffer_.size()
                       << " bytes of partial data";
    }
  }

  read_in_progress_ = false;
  if (running_.load()) {
    network_thread_->PostDelayedTask(
        [this]() {
          if (running_.load())
            async_read();
        },
        webrtc::TimeDelta::Millis(no_more_data ? 100 : 10));
  }
}

// New reconnect method
void MediaSoupWrapper::attempt_reconnect() {
  APP_LOG(AS_INFO) << "Attempting to reconnect to WebSocket server";
  stop_listening();
  if (sockfd != -1) {
    ::close(sockfd);
    sockfd = -1;
  }
  if (ssl) {
    SSL_free(ssl);
    ssl = nullptr;
  }

  if (Connect()) {
    APP_LOG(AS_INFO) << "Reconnected successfully, restarting listener";
    start_listening(message_callback_);
    // Reinitialize Mediasoup state
    if (!fully_initialized_) {
      get_router_rtp_capabilities();
    }
    if (!joined_) {
      join_room();
    }
  } else {
    APP_LOG(AS_ERROR) << "Reconnect failed, retrying in 2 seconds";
    network_thread_->PostDelayedTask([this]() { attempt_reconnect(); },
                                     webrtc::TimeDelta::Seconds(2));
  }
}

void MediaSoupWrapper::force_sync_read(int expected_id) {
  APP_LOG(AS_INFO) << "force_sync_read: Attempting synchronous read for ID: "
                   << expected_id;

  char temp[8192];
  int total_bytes_read = 0;
  int attempts = 0;
  const int max_attempts = 10;  // Increase attempts for more reliability

  while (attempts < max_attempts) {
    int bytes_read = 0;
    if (use_ssl_) {
      if (ssl) {
        bytes_read = SSL_read(ssl, temp + total_bytes_read,
                              sizeof(temp) - total_bytes_read);
        if (bytes_read <= 0) {
          int ssl_err = SSL_get_error(ssl, bytes_read);
          if (ssl_err == SSL_ERROR_WANT_READ ||
              ssl_err == SSL_ERROR_WANT_WRITE) {
            APP_LOG(AS_INFO)
                << "force_sync_read: SSL_read would block, attempt "
                << attempts + 1 << "/" << max_attempts;
            std::this_thread::sleep_for(
                std::chrono::milliseconds(100));  // Increase delay
            attempts++;
            continue;
          } else {
            APP_LOG(AS_ERROR) << "force_sync_read: SSL_read error: "
                              << ERR_error_string(ERR_get_error(), nullptr);
            break;
          }
        }
      } else {
        APP_LOG(AS_ERROR) << "force_sync_read: SSL object is null";
        break;
      }
    } else {
      bytes_read = recv(sockfd, temp + total_bytes_read,
                        sizeof(temp) - total_bytes_read, 0);
      if (bytes_read < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          APP_LOG(AS_INFO) << "force_sync_read: recv would block, attempt "
                           << attempts + 1 << "/" << max_attempts;
          std::this_thread::sleep_for(
              std::chrono::milliseconds(100));  // Increase delay
          attempts++;
          continue;
        } else {
          APP_LOG(AS_ERROR)
              << "force_sync_read: Read failed, errno: " << strerror(errno);
          break;
        }
      } else if (bytes_read == 0) {
        APP_LOG(AS_INFO) << "force_sync_read: Connection closed";
        running_ = false;
        if (message_callback_)
          message_callback_("DISCONNECTED");
        break;
      }
    }

    if (bytes_read > 0) {
      APP_LOG(AS_INFO) << "force_sync_read: Received " << bytes_read
                       << " bytes on attempt " << attempts + 1;
      total_bytes_read += bytes_read;
      {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        buffer_.append(temp, bytes_read);
      }
      std::vector<std::string> messages = process_websocket_frames();
      for (const auto& message : messages) {
        if (message_callback_ && !message.empty()) {
          APP_LOG(AS_INFO) << "force_sync_read: Processing message: "
                           << message;
          message_callback_(message);
          // Check if this is the expected response
          Json::Value json_message;
          Json::Reader reader;
          if (reader.parse(message, json_message) &&
              json_message.isMember("id")) {
            int received_id = json_message["id"].asInt();
            if (expected_id != -1 && received_id == expected_id) {
              APP_LOG(AS_INFO)
                  << "force_sync_read: Received expected response for ID "
                  << expected_id << ", stopping";
              return;  // Exit once we get the expected response
            }
          }
        }
      }
    }
    attempts++;
  }

  APP_LOG(AS_WARNING) << "force_sync_read: No expected response after "
                      << max_attempts << " attempts";
  // Schedule async read as fallback
  if (network_thread_) {
    network_thread_->PostTask(
        [this]() {
          if (running_.load() && !read_in_progress_.load()) {
            APP_LOG(AS_INFO)
                << "force_sync_read: Scheduling async read as fallback";
            async_read();
          }
        });
  }
}

void MediaSoupWrapper::start_listening(std::function<void(const std::string&)> callback) {
  std::lock_guard<std::mutex> lock(thread_mutex_);
  APP_LOG(AS_INFO) << "start_listening: Starting, running_: "
                   << running_.load();
  if (running_.exchange(true)) {
    APP_LOG(AS_WARNING) << "start_listening: Already running";
    return;
  }
  if (!network_thread_) {
    APP_LOG(AS_ERROR) << "start_listening: Network thread not set";
    running_ = false;
    return;
  }
  message_callback_ = callback;
  buffer_.clear();
  APP_LOG(AS_INFO) << "start_listening: Scheduling initial async_read";

  // Start keep-alive pings
  network_thread_->PostDelayedTask(
      [this]() {
        if (running_.load() && is_connected()) {
          send_ping();
          network_thread_->PostDelayedTask(
              [this]() { start_listening(message_callback_); },
              webrtc::TimeDelta::Seconds(5));  // Ping every 5 seconds
        }
      },
      webrtc::TimeDelta::Seconds(10));

  network_thread_->PostTask([this]() {
    APP_LOG(AS_INFO) << "start_listening: Initial async_read posted";
    async_read();
  });
}

bool MediaSoupWrapper::query_room(const std::string& roomId) {
  Json::Value query;
  query["method"] = "queryRoom";
  query["target"] = "room";
  query["roomId"] = roomId;
  query["id"] = 1;

  Json::StreamWriterBuilder builder;
  std::string json_string = Json::writeString(builder, query);
  return send_websocket_frame(json_string);
}

bool MediaSoupWrapper::join_room() {
  if (joined_) {
    return true;
  }

  // Create join request
  Json::Value request;
  request["request"] = true;
  request["id"] = generate_request_id("join");
  join_request_id_ = request["id"].asInt();
  request["method"] = "join";

  Json::Value data;
  data["displayName"] = "WebRTCsays.ai";
  data["device"] = Json::Value();
  data["device"]["flag"] = "chrome";
  data["device"]["name"] = "Chrome";
  data["device"]["version"] = "132.0.0.0";
  
  // Create RTP capabilities based on the browser template (audio only)
  Json::Value rtpCapabilities;
  
  // Audio codec - opus
  Json::Value audioCodec;
  audioCodec["mimeType"] = "audio/opus";
  audioCodec["kind"] = "audio";
  audioCodec["preferredPayloadType"] = 100;
  audioCodec["clockRate"] = 48000;
  audioCodec["channels"] = 2;
  
  // Audio codec parameters
  Json::Value codecParams;
  codecParams["minptime"] = 10;
  codecParams["useinbandfec"] = 1;
  audioCodec["parameters"] = codecParams;
  
  // Audio codec RTCP feedback
  Json::Value rtcpFeedback1;
  rtcpFeedback1["type"] = "transport-cc";
  rtcpFeedback1["parameter"] = "";
  audioCodec["rtcpFeedback"].append(rtcpFeedback1);
  
  Json::Value rtcpFeedback2;
  rtcpFeedback2["type"] = "nack";
  rtcpFeedback2["parameter"] = "";
  audioCodec["rtcpFeedback"].append(rtcpFeedback2);
  
  rtpCapabilities["codecs"].append(audioCodec);
  
  // Header extensions - include only audio-related ones
  Json::Value midExt;
  midExt["kind"] = "audio";
  midExt["uri"] = "urn:ietf:params:rtp-hdrext:sdes:mid";
  midExt["preferredId"] = 1;
  midExt["preferredEncrypt"] = false;
  midExt["direction"] = "sendrecv";
  rtpCapabilities["headerExtensions"].append(midExt);
  
  Json::Value absSendTimeExt;
  absSendTimeExt["kind"] = "audio";
  absSendTimeExt["uri"] = "http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time";
  absSendTimeExt["preferredId"] = 4;
  absSendTimeExt["preferredEncrypt"] = false;
  absSendTimeExt["direction"] = "sendrecv";
  rtpCapabilities["headerExtensions"].append(absSendTimeExt);
  
  Json::Value audioLevelExt;
  audioLevelExt["kind"] = "audio";
  audioLevelExt["uri"] = "urn:ietf:params:rtp-hdrext:ssrc-audio-level";
  audioLevelExt["preferredId"] = 10;
  audioLevelExt["preferredEncrypt"] = false;
  audioLevelExt["direction"] = "sendrecv";
  rtpCapabilities["headerExtensions"].append(audioLevelExt);
  
  data["rtpCapabilities"] = rtpCapabilities;
  
  // Add SCTP capabilities
  Json::Value sctp_capabilities;
  sctp_capabilities["numStreams"] = Json::Value();
  sctp_capabilities["numStreams"]["OS"] = 1024;
  sctp_capabilities["numStreams"]["MIS"] = 1024;
  data["sctpCapabilities"] = sctp_capabilities;
  
  data["peerId"] = peer_id_;
  data["roomId"] = room_id_;

  request["data"] = data;

  // Send the request
  APP_LOG(AS_INFO) << "Sending join request with id " << join_request_id_ 
                  << " using browser template (audio only)";
  return send_websocket_frame(Json::writeString(Json::StreamWriterBuilder(), request));
}

bool MediaSoupWrapper::get_router_rtp_capabilities() {
  Json::Value request;
  request["request"] = true;
  request["method"] = "getRouterRtpCapabilities";
  request["target"] = "peer";
  request["peerId"] = peer_id_;
  request["roomId"] = room_id_;
  request["id"] = generate_request_id("getRouterRtpCapabilities");

  Json::StreamWriterBuilder builder;
  std::string json_string = Json::writeString(builder, request);
  rtp_caps_request_id_ = request["id"].asInt();  // Store the join request ID
  APP_LOG(AS_INFO) << "Sending getRouterRtpCapabilities request id "
                   << rtp_caps_request_id_ << ": " << json_string;
  return send_websocket_frame(json_string);
}

bool MediaSoupWrapper::create_producer_transport() {
  Json::Value request;
  request["request"] = true;
  request["method"] = "createWebRtcTransport";
  request["id"] = generate_request_id("createProducerTransport");
  request["data"]["forceTcp"] = false;
  request["data"]["producing"] = true;
  request["data"]["consuming"] = false;
  request["data"]["sctpCapabilities"]["numStreams"]["OS"] = 1024;
  request["data"]["sctpCapabilities"]["numStreams"]["MIS"] = 1024;

  Json::StreamWriterBuilder builder;
  std::string json_string = Json::writeString(builder, request);
  producer_request_id_ =
      request["id"].asInt();  // Store the producer request ID
  APP_LOG(AS_INFO) << "Sending producer transport request id "
                   << producer_request_id_ << ": " << json_string;
  return send_websocket_frame(json_string);
}

bool MediaSoupWrapper::create_consumer_transport() {
  Json::Value request;
  request["request"] = true;
  request["method"] = "createWebRtcTransport";
  request["id"] = generate_request_id("createConsumerTransport");
  
  // Create data object explicitly
  Json::Value data;
  data["forceTcp"] = false;
  data["producing"] = false;
  data["consuming"] = true;
  
  // Add SCTP capabilities
  Json::Value sctp_capabilities;
  Json::Value num_streams;
  num_streams["OS"] = 1024;
  num_streams["MIS"] = 1024;
  sctp_capabilities["numStreams"] = num_streams;
  data["sctpCapabilities"] = sctp_capabilities;
  
  // Add data to request
  request["data"] = data;

  Json::StreamWriterBuilder builder;
  std::string json_string = Json::writeString(builder, request);
  consumer_request_id_ = request["id"].asInt();  // Store the consumer request ID
  
  APP_LOG(AS_INFO) << "Sending consumer transport request id "
                   << consumer_request_id_ << ": " << json_string;
  return send_websocket_frame(json_string);
}

bool MediaSoupWrapper::connect_transport(const bool is_consumer,
                       const std::string& transport_id,
                       const Json::Value& dtAS_params) {
  Json::Value request;
  request["request"] = true;
  request["method"] = "connectWebRtcTransport";
  std::string mode = is_consumer ? "Consumer" : "Producer";
  request["id"] = generate_request_id("connect" + mode + "Transport");
  request["data"]["transportId"] = transport_id;
  request["data"]["dtlsParameters"] = dtAS_params;
  request["data"]["dtlsParameters"]["role"] = "client";

  Json::StreamWriterBuilder builder;
  std::string json_string = Json::writeString(builder, request);
  APP_LOG(AS_INFO) << "Before connect_transport as consumer: " << is_consumer;
  int connect_request_id_ =
      request["id"].asInt();  // Store the connect request ID
  if (is_consumer) {
    consumer_connect_request_id_ = connect_request_id_;
  } else {
    producer_connect_request_id_ = connect_request_id_;
  }
  APP_LOG(AS_INFO) << "Sending connectWebRtcTransport for "
                   << (is_consumer ? "consumer" : "producer")
                   << " request id: " << connect_request_id_ << ": "
                   << json_string;
  return send_websocket_frame(json_string);
}

bool MediaSoupWrapper::produce_audio(const std::string& transportId,
                   uint32_t send_ssrc,
                   std::string send_mid) {
  Json::Value request;
  request["request"] = true;
  request["method"] = "produce";
  request["id"] = generate_request_id("produceAudio");
  request["data"]["transportId"] = producer_transport_id_;
  request["data"]["kind"] = "audio";

  Json::Value& rtpParams = request["data"]["rtpParameters"];

  Json::Value codec;
  codec["mimeType"] = "audio/opus";
  codec["payloadType"] = 100;
  codec["clockRate"] = 48000;
  codec["channels"] = 2;
  codec["parameters"]["minptime"] = 10;
  codec["parameters"]["useinbandfec"] = 1;
  rtpParams["codecs"].append(codec);

  Json::Value encoding;
  encoding["dtx"] = false;
  if (send_ssrc != 0) {
    encoding["ssrc"] = send_ssrc;
    APP_LOG(AS_INFO) << "MediaSoupWrapper::produce_audio - Explicitly setting SSRC in produce request: " << send_ssrc;
  } else {
    APP_LOG(AS_INFO) << "MediaSoupWrapper::produce_audio - Not setting SSRC in produce request (will be auto-detected by mediasoup).";
  }
  rtpParams["encodings"].append(encoding);

  // Add header extensions
  Json::Value midExt;
  midExt["uri"] = "urn:ietf:params:rtp-hdrext:sdes:mid";
  midExt["id"] = 1;
  midExt["encrypt"] = false;
  rtpParams["headerExtensions"].append(midExt);

  // Add audio level extension
  Json::Value levelExt;
  levelExt["uri"] = "urn:ietf:params:rtp-hdrext:ssrc-audio-level";
  levelExt["id"] = 10;
  levelExt["encrypt"] = false;
  rtpParams["headerExtensions"].append(levelExt);

  // Use a different mid for sending
  rtpParams["mid"] = send_mid;

  Json::StreamWriterBuilder builder;
  builder["commentStyle"] = "None";
  builder["indentation"] = "";

  std::string json_string = Json::writeString(builder, request);
  produce_request_id_ = request["id"].asInt();

  APP_LOG(AS_INFO) << "Sending produce request with id: " << produce_request_id_
                   << ": " << json_string;
                   
  return send_websocket_frame(json_string);
}

bool MediaSoupWrapper::resume_consumer(const std::string& consumer_id) {
  Json::Value request;
  request["request"] = true;
  request["method"] = "resumeConsumer";
  request["id"] = generate_request_id("resumeConsumer");
  request["data"]["consumerId"] = consumer_id;

  Json::StreamWriterBuilder builder;
  std::string json_string = Json::writeString(builder, request);
  resume_consumer_request_id_ = request["id"].asInt();
  return send_websocket_frame(json_string);
}

void MediaSoupWrapper::stop_listening() {
  std::lock_guard<std::mutex> lock(thread_mutex_);
  APP_LOG(AS_INFO) << "Stopping listener, current running state: "
                   << running_.load();
  if (running_.exchange(false)) {
    APP_LOG(AS_INFO) << "Stopping WebSocket listener for peer: " << peer_id_;
    Json::Value notify;
    notify["type"] = "audioStopped";
    notify["peerId"] = peer_id_;
    notify["id"] = -1;
    Json::StreamWriterBuilder builder;
    std::string notify_string = Json::writeString(builder, notify);
    send_websocket_frame(notify_string);
    fully_initialized_ = false;  // Reset to allow re-initialization if needed
  }
}

std::string MediaSoupWrapper::get_peer_id() {
  return generate_peer_id();
}

bool MediaSoupWrapper:: has_rtp_capabilities() const {
  return rtp_capabilities_received_;
}

void MediaSoupWrapper::set_rtp_capabilities(const Json::Value& capabilities) {
  rtp_capabilities_ = capabilities;
  rtp_capabilities_received_ = true;
}

// Add method to generate request IDs
int MediaSoupWrapper::generate_request_id(const std::string& method) {
  std::lock_guard<std::mutex> lock(pending_requests_mutex_);
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<> dis(100000, 999999);
  int request_id = dis(gen);
  APP_LOG(AS_INFO) << "Generated request id: " << request_id << " for method: " << method;
  request_info_map_[request_id] = method;
  return request_id;
}

bool MediaSoupWrapper::is_connected() {
  if (sockfd == -1)
    return false;

  // Add a timeout to avoid too frequent checks
  static auto last_check = std::chrono::steady_clock::now();
  auto now = std::chrono::steady_clock::now();
  if (now - last_check < std::chrono::seconds(1)) {
    return true;  // Return cached result for 1 second
  }
  last_check = now;

  // Check if socket is still valid
  int error = 0;
  socklen_t len = sizeof(error);
  int retval = getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &error, &len);

  if (retval != 0 || error != 0) {
    APP_LOG(AS_ERROR) << "Socket error detected: " << strerror(error);
    return false;
  }

  return true;
}

bool MediaSoupWrapper::Connect() {
  APP_LOG(AS_INFO) << "Connecting directly to WebSocket server at "
                   << target_host_ << ":" << target_port_;

  if (sockfd != -1) {
    ::close(sockfd);
    sockfd = -1;
  }

  if (!connect(target_host_, target_port_)) {
    return false;
  }

  if (!send_http_request("/")) {
    return false;
  }
  return true;  // Don't send initial requests or set fully_initialized_ here
}

bool MediaSoupWrapper::consume_audio(const std::string& transport_id,
                   const std::string& producer_id,
                   const std::string& consumer_id,
                   const Json::Value& rtp_parameters) {
  APP_LOG(AS_INFO) << "consume_audio called [consumerId: " << consumer_id
                   << ", producerId: " << producer_id
                   << "] - handled by server";
  return true;  // Server initiates consumers via "newConsumer"    return
                // true;
}
