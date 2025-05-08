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

#pragma once

#include <fcntl.h>
#include <json/json.h>
#include <netdb.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <random>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "direct.h"


class MediaSoupWrapper {
  // This class wraps a WebSocket connection to a Mediasoup server.
  // It handles the connection, sending and receiving messages, and
  // managing the SSL context if SSL is used.
 public:
  MediaSoupWrapper(const std::string& cert_file = "", bool use_ssl = false);
  ~MediaSoupWrapper();

  void set_key_file(const std::string& key_file);
  void set_network_thread(rtc::Thread* thread);
  bool connect(const std::string& host, const std::string& port);
  bool send_http_request(const std::string& path);
  bool send_websocket_frame(const std::string& message);
  std::vector<std::string> process_websocket_frames();
  void async_read();
  void attempt_reconnect();
  void force_sync_read(int expected_id = -1);
  void start_listening(std::function<void(const std::string&)> callback);
  bool query_room(const std::string& roomId);
  bool join_room();
  bool get_router_rtp_capabilities();
  bool create_producer_transport();
  bool create_consumer_transport();
  bool connect_transport(const bool is_consumer,
                         const std::string& transport_id,
                         const Json::Value& dtls_params);
  bool produce_audio(const std::string& transportId,
                     uint32_t send_ssrc,
                     std::string send_mid);
  bool resume_consumer(const std::string& consumer_id);
  void stop_listening();
  std::string get_peer_id();
  bool has_rtp_capabilities() const;
  void set_rtp_capabilities(const Json::Value& capabilities);
  int generate_request_id(const std::string& method);
  bool is_connected();
  bool Connect();
  bool consume_audio(const std::string& transport_id,
                     const std::string& producer_id,
                     const std::string& consumer_id,
                     const Json::Value& rtp_parameters);

  int sockfd;
  SSL_CTX* ctx;
  SSL* ssl;
  std::atomic<bool> running;
  std::mutex ssl_mutex;
  std::thread listener_thread;
  Json::Value rtp_capabilities_;
  bool rtp_capabilities_received_ = false;
  std::string host_;
  std::string port_;
  std::string room_id_;
  std::string peer_id_;
  std::string producer_id_;
  std::string producer_transport_id_;
  std::string consumer_transport_id_;
  std::map<std::string, Json::Value> pending_consumers_;
  std::mutex consumers_mutex_;
  bool consumer_transport_connected_ = false;
  bool producer_transport_connected_ = false;

  struct Producer {
    std::string id;
    uint32_t ssrc;

    Producer() : ssrc(0) {}
    Producer(std::string _id, uint32_t _ssrc) : 
      id(_id), ssrc(_ssrc) {}
  };

  struct Consumer {
    std::string id;
    std::string producer_id;
    Json::Value data;
    uint32_t ssrc;

    Consumer() : ssrc(0) {}
    Consumer(std::string _id,
             std::string _producer_id,
             const Json::Value& _data,
             uint32_t _ssrc)
        : id(_id), producer_id(_producer_id), data(_data), ssrc(_ssrc) {}
  };

  std::map<std::string, Producer> producers_;
  std::map<std::string, Consumer> consumers_;
  std::function<void(const std::function<void()>&)> receive_and_process;
  bool use_ssl_ = false;
  std::string target_host_ = "v3demo.mediasoup.org";
  std::string target_port_ = "4443";
  std::mutex thread_mutex_;
  std::atomic<bool> running_{false};
  rtc::Thread* network_thread_ = nullptr;
  std::function<void()> listener_task_;
  bool fully_initialized_ = false;
  std::string buffer_;
  std::function<void(const std::string&)> message_callback_;
  std::mutex buffer_mutex_;
  std::condition_variable data_available_;
  std::atomic<bool> read_in_progress_{false};
  std::string cert_file_path_;
  std::string key_file_path_;
  int rtp_caps_request_id_ = -1;
  int producer_request_id_ = -1;
  int consumer_request_id_ = -1;
  int consumer_connect_request_id_ = -1;
  int producer_connect_request_id_ = -1;
  int join_request_id_ = -1;
  int produce_request_id_ = -1;
  int restart_ice_request_id_ = -1;
  int resume_consumer_request_id_ = -1;
  bool joined_ = false;
  uint32_t recv_ssrc_ = 0;
  uint32_t send_ssrc_ = 0;
  int next_request_id_ = 1;
  int last_request_id_ = -1;
  std::string last_request_method_;
  static constexpr int kAudioSampleRate = 48000;
  static constexpr int kAudioChannels = 2;
  std::map<int, std::function<void(const Json::Value&)>> pending_requests_;
  std::mutex pending_requests_mutex_;

  std::string generate_websocket_key();
  std::string generate_peer_id();
  std::string base64_encode(const std::string& in);
  void send_ping();
  void send_pong_frame(const std::vector<unsigned char>& ping_payload);
  int LastRequestId(const std::string& method) const;

  std::map<int, std::string> request_info_map_;

};
