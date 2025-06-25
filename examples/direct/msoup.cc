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
  if (ws_client_) {
    ws_client_->send_ping();
  } else {
    APP_LOG(AS_ERROR) << "WebSocket client not initialized in send_ping";
  }
}

void MediaSoupWrapper::send_pong_frame(const std::vector<unsigned char>& ping_payload) {
  if (ws_client_) {
    ws_client_->send_pong_frame(ping_payload);
  } else {
    APP_LOG(AS_ERROR) << "WebSocket client not initialized in send_pong_frame";
  }
}

MediaSoupWrapper::MediaSoupWrapper(const std::string& cert_file, bool use_ssl)
    : use_ssl_(use_ssl),
      cert_file_path_(cert_file) {
  ws_client_ = std::make_unique<WebSocketClient>();
}

void MediaSoupWrapper::set_key_file(const std::string& key_file) {
  key_file_path_ = key_file;
}

void MediaSoupWrapper::set_network_thread(rtc::Thread* thread) {
  network_thread_ = thread;
  if (ws_client_) {
    ws_client_->set_network_thread(thread);
  }
}

MediaSoupWrapper::~MediaSoupWrapper() {
  stop_listening();
  if (ws_client_) {
    ws_client_->disconnect();
  }
}

bool MediaSoupWrapper::connect(const std::string& host, const std::string& port) {
  if (!ws_client_) {
    APP_LOG(AS_ERROR) << "WebSocket client not initialized";
    return false;
  }

  // Configure WebSocket client
  WebSocketClient::Config config;
  config.host = host;
  config.port = port;
  config.use_ssl = use_ssl_;
  config.cert_file_path = cert_file_path_;
  config.key_file_path = key_file_path_;

  return ws_client_->connect(config);
}

bool MediaSoupWrapper::send_http_request(const std::string& path) {
  if (!ws_client_) {
    APP_LOG(AS_ERROR) << "WebSocket client not initialized";
    return false;
  }
  
  // Configure WebSocket handshake with custom headers
  WebSocketClient::Config config;
  config.host = target_host_;
  config.port = target_port_;
  config.use_ssl = use_ssl_;
  config.cert_file_path = cert_file_path_;
  config.key_file_path = key_file_path_;
  
  std::string full_path = "/?roomId=" + room_id_ + "&peerId=" + peer_id_;
  config.headers["path"] = full_path;
  config.headers["User-Agent"] = "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/132.0.0.0 Safari/537.36";
  config.headers["Origin"] = "https://v3demo.mediasoup.org";
  config.headers["Sec-WebSocket-Extensions"] = "permessage-deflate; client_max_window_bits";
  config.headers["Sec-WebSocket-Protocol"] = "protoo";
  
  return ws_client_->send_websocket_handshake_with_headers();
}

bool MediaSoupWrapper::send_websocket_frame(const std::string& message) {
  if (!ws_client_) {
    APP_LOG(AS_ERROR) << "WebSocket client not initialized";
    return false;
  }
  return ws_client_->send_message(message);
}





// New reconnect method
void MediaSoupWrapper::attempt_reconnect() {
  APP_LOG(AS_INFO) << "Attempting to reconnect to WebSocket server";
  stop_listening();
  
  if (ws_client_) {
    ws_client_->disconnect();
  }

  if (connect(host_, port_)) {
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
    if (network_thread_) {
      network_thread_->PostDelayedTask([this]() { attempt_reconnect(); },
                                       webrtc::TimeDelta::Seconds(2));
    }
  }
}

// force_sync_read method removed - functionality delegated to WebSocketClient

void MediaSoupWrapper::start_listening(std::function<void(const std::string&)> callback) {
  message_callback_ = callback;
  if (!ws_client_) {
    APP_LOG(AS_ERROR) << "WebSocket client not initialized";
    return;
  }
  
  ws_client_->set_message_callback(callback);
  ws_client_->start_listening();
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
  if (ws_client_) {
    ws_client_->stop_listening();
  }
  
  // Send stop notification
  Json::Value notify;
  notify["type"] = "audioStopped";
  notify["peerId"] = peer_id_;
  notify["id"] = -1;
  Json::StreamWriterBuilder builder;
  std::string notify_string = Json::writeString(builder, notify);
  send_websocket_frame(notify_string);
  fully_initialized_ = false;  // Reset to allow re-initialization if needed
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
  if (!ws_client_) {
    return false;
  }
  return ws_client_->is_connected();
}

bool MediaSoupWrapper::Connect() {
  APP_LOG(AS_INFO) << "Connecting directly to WebSocket server at "
                   << target_host_ << ":" << target_port_;

  if (!ws_client_) {
    APP_LOG(AS_ERROR) << "WebSocket client not initialized";
    return false;
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
