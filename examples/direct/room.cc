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

#include "modules/audio_device/include/audio_device.h"  // For AudioDeviceModule
#include "pc/rtp_receiver.h"  // Needed for RtpReceiverInternal
#include "rtc_base/crypto_random.h"
#include "rtc_base/system/rtc_export.h"

#include "room.h"
#include "msoup.h"
#include "option.h"

#include <utility>
#include <vector>

#include "absl/strings/string_view.h" // Added for absl::string_view
#include "absl/strings/match.h"      // For absl::EqualsIgnoreCase
#include "api/audio_codecs/audio_decoder_factory.h"
#include "api/audio_codecs/audio_encoder_factory.h"
#include "api/rtp_parameters.h"      // For RtpCodecCapability
#include <regex>
#include <string>
#include <sstream>
#include <algorithm>

// Define an AudioSink class near the top of room.cc
class AudioSink : public webrtc::AudioTrackSinkInterface {
 public:
  virtual void OnData(
      const void* audio_data,
      int bits_per_sample,
      int sample_rate,
      size_t number_of_channels,
      size_t number_of_frames,
      std::optional<int64_t> /* absolute_capture_timestamp_ms */) override {
    if (++frame_counter % 50 == 0) {
      APP_LOG(AS_INFO) << "Audio format:"
                       << " bits=" << bits_per_sample << " rate=" << sample_rate
                       << " channels=" << number_of_channels
                       << " frames=" << number_of_frames << " buffer_size="
                       << (number_of_frames * number_of_channels *
                           (bits_per_sample / 8));

      // Log first few samples to verify stereo
      const int16_t* samples = static_cast<const int16_t*>(audio_data);
      if (samples && number_of_frames > 0) {
        std::stringstream ss;
        ss << "First few samples:";
        for (size_t i = 0;
             i < std::min(size_t(10), number_of_frames * number_of_channels);
             i++) {
          ss << " " << samples[i];
        }
        APP_LOG(AS_INFO) << ss.str();
      }

    }
    // If your audio is being received but not playing, you could try
    // creating a connection to your audio output device here and
    // directly play the samples
    // This would be platform-specific code
    received_audio_ = true;

    return;
  }

  void SetSsrc(uint32_t ssrc) {
    ssrc_ = ssrc;
    APP_LOG(AS_INFO) << "AudioSink configured to filter for SSRC: " << ssrc;
  }

  uint32_t ssrc() const { return ssrc_; }

  void SetRoomCaller(RoomCaller* room_caller) { room_caller_ = room_caller; }

 private:
  uint32_t ssrc_ = 0;
  bool received_audio_ = false;
  int64_t frame_counter = 0;
  RoomCaller* room_caller_ = nullptr;
};

class StatsCallback : public webrtc::RTCStatsCollectorCallback {
 public:
  explicit StatsCallback(
      std::function<void(
          const rtc::scoped_refptr<const webrtc::RTCStatsReport>&)> callback)
      : callback_(callback) {}

  void OnStatsDelivered(
      const rtc::scoped_refptr<const webrtc::RTCStatsReport>& report) override {
    callback_(report);
  }

 private:
  std::function<void(const rtc::scoped_refptr<const webrtc::RTCStatsReport>&)>
      callback_;
};

// DataChannelObserver definition (can be nested inside RoomCaller or
// standalone)
class MediasoupDataChannelObserver : public webrtc::DataChannelObserver {
 public:
  MediasoupDataChannelObserver(
      rtc::scoped_refptr<webrtc::DataChannelInterface> channel)
      : channel_(channel) {
    channel_->RegisterObserver(this);
  }
  ~MediasoupDataChannelObserver() override {
    if (channel_) {
      channel_->UnregisterObserver();
    }
  }

  void OnStateChange() override {
    APP_LOG(AS_INFO) << "DataChannel [" << channel_->label() << ":"
                     << channel_->id() << "] state changed: "
                     << webrtc::DataChannelInterface::DataStateString(
                            channel_->state());
    if (channel_->state() == webrtc::DataChannelInterface::kOpen) {
      // Example: Send a message when the channel opens
      std::string message = "Hello from client data channel!";
      webrtc::DataBuffer buffer(
          rtc::CopyOnWriteBuffer(message.c_str(), message.length()),
          true /* binary */);
      channel_->Send(buffer);
      APP_LOG(AS_INFO) << "Sent initial message on DataChannel ["
                       << channel_->label() << "]";
    }
  }

  void OnMessage(const webrtc::DataBuffer& buffer) override {
    // Important: Check if data is string or binary
    if (buffer.binary) {
      APP_LOG(AS_INFO) << "DataChannel [" << channel_->label() << ":"
                       << channel_->id()
                       << "] received binary message of size: "
                       << buffer.data.size();
      // Process binary data
    } else {
      std::string message(buffer.data.cdata<char>(), buffer.data.size());
      APP_LOG(AS_INFO) << "DataChannel [" << channel_->label() << ":"
                       << channel_->id()
                       << "] received string message: " << message;
      // Process string data
    }
  }

  void OnBufferedAmountChange(uint64_t previous_amount) override {
    APP_LOG(AS_VERBOSE) << "DataChannel [" << channel_->label() << ":"
                        << channel_->id() << "] buffered amount changed from "
                        << previous_amount << " to "
                        << channel_->buffered_amount();
  }

 private:
  rtc::scoped_refptr<webrtc::DataChannelInterface> channel_;
};


RoomCaller::RoomCaller(Options opts)
    : DirectApplication(opts),
      opts_(opts),
      wss_(nullptr),
      // demuxer_initialized_event_(false, false),
      pending_producer_data_(Json::Value()),
      pending_consumer_data_(Json::Value()),
      pending_ice_candidates_(),
      producer_mediasoup_ice_candidates_(),
      producer_mediasoup_ice_parameters_(),
      consumer_mediasoup_ice_candidates_(),
      consumer_mediasoup_ice_parameters_(),
      producer_created_(false),
      sdp_negotiation_started_(false),
      reconnect_attempts_(0) {
}

bool RoomCaller::Initialize() {

  APP_LOG(AS_INFO) << "RoomCaller::Initialize()";
  DirectApplication::Initialize();

  if (!ws_thread()) {
    APP_LOG(AS_ERROR) << "DirectApplication not initialized";
    return false;
  }

  if (!ws_thread()->Start()) {
    APP_LOG(AS_ERROR) << "Failed to start WebSocket thread";
    return false;
  }

  APP_LOG(AS_INFO) << "ws_thread()->Start() succeeded."; // <-- ADD THIS

  // Create MediaSoupWrapper first
  wss_ = std::make_unique<MediaSoupWrapper>(opts_.webrtc_cert_path, true);
  wss_->set_network_thread(ws_thread());
  wss_->set_key_file(opts_.webrtc_key_path);

  // Set up connection parameters
  std::string ip;
  int port = 0;
  ParseIpAndPort(opts_.address, ip, port);
  if (port == 0) {
    port = 4443;
  }

  wss_->target_host_ = ip.empty() ? "v3demo.mediasoup.org" : ip;
  wss_->target_port_ = (ip != "127.0.0.1") ? "4443" : std::to_string(port);
  wss_->peer_id_ = wss_->generate_peer_id();
  wss_->room_id_ = "room101";

  if (opts_.encryption && !certificate_) {
    certificate_ = DirectLoadCertificateFromEnv(opts_);
    UpdateCertificateStats();
    if(certificate_stats() != nullptr) {
    APP_LOG(AS_INFO) << "Using certificate with fingerprint: "
                     << certificate_stats_->fingerprint;  
    }
  }

  // Don't connect here - let Run() handle it
  running_ = true;
  return true;
}

RoomCaller::~RoomCaller() {
  // Clean up resources if needed
  APP_LOG(AS_INFO) << "Destroying RoomCaller";

  running_ = false;
  if (wss_) {
    wss_->stop_listening();
    if (wss_->sockfd != -1) {
      APP_LOG(AS_INFO) << "Closing socket FD in destructor: " << wss_->sockfd;
      ::close(wss_->sockfd);
      wss_->sockfd = -1;
    }
  }

  Shutdown();
  APP_LOG(AS_INFO) << "RoomCaller destroyed";
}

void RoomCaller::Run() {
  APP_LOG(AS_INFO) << "RoomCaller::Run() - Starting event loop";

  // Start connection on ws_thread()
  ws_thread()->PostTask([this]() {
    APP_LOG(AS_INFO) << "ws_thread_ task execution started."; // <-- ADD THIS
    APP_LOG(AS_INFO) << "Attempting wss_->Connect()...";      // <-- ADD THIS
    if (wss_->Connect()) {
      APP_LOG(AS_INFO) << "wss_->Connect() succeeded.";      // <-- ADD THIS
      APP_LOG(AS_INFO) << "WebSocket connected successfully";

      // Start the WebSocket listener
      wss_->start_listening(
          [this](const std::string& message) { OnWebSocketMessage(message); });

      // Now that we're listening, start the protocol
      wss_->get_router_rtp_capabilities();
      KeepAlive();
    } else {
      APP_LOG(AS_ERROR) << "wss_->Connect() FAILED.";         // <-- ADD THIS
      APP_LOG(AS_ERROR) << "Failed to connect to WebSocket server";
    }
  });

  // Call the parent class's Run() which contains the message processing loop
  //DirectApplication::Run();
  // while (running_) { // Assuming 'running_' is accessible and controlled by Shutdown()
  //     rtc::Thread::SleepMs(100); // Sleep for 100ms
  // }  
}

bool RoomCaller::Connect() {
  // Ensure the RoomCaller event loop and WebSocket connection start when Connect() is called
  Run();
  return true;
}

void RoomCaller::Start() {}

void RoomCaller::OnWebSocketMessage(const std::string& message) {
  APP_LOG(AS_VERBOSE) << "Received message: " << message
                      << ", thread: " << std::this_thread::get_id();

  if (message == "DISCONNECTED") {
    APP_LOG(AS_ERROR) << "WebSocket disconnected, attempting reconnect...";
    ReconnectWebSocket();
    return;
  }

  Json::Value json_message;
  Json::Reader reader;
  if (!reader.parse(message, json_message)) {
    APP_LOG(AS_ERROR) << "Failed to parse JSON message: " << message;
    return;
  }

  if (json_message.isMember("request") && json_message["request"].asBool()) {
    APP_LOG(AS_VERBOSE) << "Message type: request";
    HandleRequest(json_message);
  } else if (json_message.isMember("response") &&
             json_message["response"].asBool()) {
    APP_LOG(AS_VERBOSE) << "Message type: response";
    HandleResponse(json_message);
  } else if (json_message.isMember("notification") &&
             json_message["notification"].asBool()) {
    APP_LOG(AS_VERBOSE) << "Message type: notification";
    HandleNotification(json_message);
  } else {
    APP_LOG(AS_WARNING) << "Unknown message type: " << message;
  }
}

void RoomCaller::KeepAlive() {
  if (running_) {
    // Schedule the next keep-alive check
    if (ws_thread()) {
      ws_thread()->PostDelayedTask(
          [this]() { KeepAlive(); },
          webrtc::TimeDelta::Seconds(
              5));  // Run every 5 seconds to keep thread alive
    }
  }
}

void RoomCaller::SendPendingRequests() {
  APP_LOG(AS_INFO) << "Resending pending requests after reconnect...";
  wss_->producer_transport_id_.clear();
  wss_->consumer_transport_id_.clear();
  pending_producer_data_ = Json::Value();
  pending_consumer_data_ = Json::Value();
  producer_mediasoup_ice_candidates_.clear();
  consumer_mediasoup_ice_candidates_.clear();
  producer_mediasoup_ice_parameters_ = Json::Value();
  consumer_mediasoup_ice_parameters_ = Json::Value();
  producer_created_ = false;
  wss_->joined_ = false;

  wss_->get_router_rtp_capabilities();  // Restart the process
}

void RoomCaller::ReconnectWebSocket() {
  reconnect_attempts_++;
  if (reconnect_attempts_ >= kMaxReconnectAttempts) {
    APP_LOG(AS_ERROR) << "Max reconnect attempts (" << kMaxReconnectAttempts
                      << ") reached, giving up.";
    Shutdown();
    return;
  }

  APP_LOG(AS_INFO) << "Reconnecting WebSocket (attempt " << reconnect_attempts_
                   << " of " << kMaxReconnectAttempts << ")...";
  wss_->stop_listening();
  if (wss_->sockfd != -1) {
    ::close(wss_->sockfd);
    wss_->sockfd = -1;
  }

  // Reset WebSocket state
  wss_->fully_initialized_ = false;
  wss_->joined_ = false;
  wss_->producer_transport_id_.clear();
  wss_->consumer_transport_id_.clear();
  pending_producer_data_ = Json::Value();
  pending_consumer_data_ = Json::Value();
  producer_created_ = false;

  if (wss_->connect(wss_->target_host_, wss_->target_port_)) {
    wss_->start_listening(
        [this](const std::string& msg) { OnWebSocketMessage(msg); });
    APP_LOG(AS_INFO) << "Reconnected successfully";
    SendPendingRequests();
  } else {
    APP_LOG(AS_ERROR) << "Reconnect failed";
    if (wss_->network_thread_) {
      wss_->network_thread_->PostDelayedTask(
          [this]() { ReconnectWebSocket(); },
          webrtc::TimeDelta::Millis(kReconnectDelayMs));
    }
  }
}

void RoomCaller::HandleNotification(const Json::Value& json_message) {
  std::string method = json_message["method"].asString();
  APP_LOG(AS_INFO) << "Handling notification: " << method;

  if (method == "mediasoup-version") {
    std::string version = json_message["data"]["version"].asString();
    APP_LOG(AS_INFO) << "Mediasoup server version: " << version;
  } else if (method == "activeSpeaker") {
    std::string peer_id = json_message["data"]["peerId"].asString();
    APP_LOG(AS_INFO) << "Active speaker: " << peer_id;
    // Handle active speaker logic if needed
    LogRtpStats();
  } else if (method == "newPeer") {
    // Handle new peer joining
    const Json::Value& data = json_message["data"];
    std::string peer_id = data["id"].asString();
    std::string display_name = data["displayName"].asString();
    APP_LOG(AS_INFO) << "Peer joined: " << peer_id << " " << display_name;

    // Check if this is a reconnection of an existing peer
    if (wss_->consumers_.count(peer_id) > 0) {
      APP_LOG(AS_INFO) << "Detected reconnection of peer: " << peer_id;
      // Handle the browser refresh case
      HandlePeerReconnection();
    }
  } else if (method == "peerClosed") {
    // Handle peer leaving
    const Json::Value& data = json_message["data"];
    std::string peer_id = data["peerId"].asString();

    APP_LOG(AS_INFO) << "Peer left: " << peer_id << " " << json_message;

    // Could clean up any stored state for this peer if needed
    if (wss_ && wss_->consumers_.count(peer_id)) {
      wss_->consumers_.erase(peer_id);
    }
  } else if (method == "producerScore" || method == "consumerScore") {
    // Just log these for debugging
    const Json::Value& data = json_message["data"];
    std::string id = method == "producerScore" ? data["producerId"].asString()
                                               : data["consumerId"].asString();
    APP_LOG(AS_INFO) << method << " for ID " << id;
  } else {
    APP_LOG(AS_INFO) << "Notification: " << method << " Data: "
                     << Json::writeString(Json::StreamWriterBuilder(),
                                          json_message["data"]);
  }
}

void RoomCaller::HandleRequest(const Json::Value& json_message) {
  std::string method = json_message["method"].asString();

  APP_LOG(AS_INFO) << "Received request method: " << method;
  APP_LOG(AS_INFO) << " data: "
                   << Json::writeString(Json::StreamWriterBuilder(),
                                        json_message);

  if (method == "newConsumer") {
    // Extract the consumer data
    const Json::Value& data = json_message["data"];
    std::string consumer_id = data["id"].asString();

    // Create and send the response immediately
    Json::Value response;
    response["response"] = true;
    response["id"] = json_message["id"];  // Use same ID as the request
    response["ok"] = true;
    response["data"] = Json::Value(Json::objectValue);  // Empty data object

    Json::StreamWriterBuilder builder;
    std::string json_string = Json::writeString(builder, response);
    wss_->send_websocket_frame(json_string);

    APP_LOG(AS_INFO) << "Sent immediate accept response for newConsumer: "
                     << consumer_id;

    HandleNewConsumer(data);

    return;  // Exit without processing further

  } else if (method == "newDataConsumer") {
    // Handle newDataConsumer request (similar pattern as newConsumer)
    const Json::Value& data = json_message["data"];
    std::string consumer_id = data["id"].asString();

    // Send accept response immediately
    Json::Value response;
    response["response"] = true;
    response["id"] = json_message["id"];
    response["ok"] = true;
    response["data"] = Json::Value(Json::objectValue);

    wss_->send_websocket_frame(
        Json::writeString(Json::StreamWriterBuilder(), response));
    APP_LOG(AS_INFO) << "Sent accept response for newDataConsumer: "
                     << consumer_id;
  } else {
    // For any unknown request, send a generic success response
    Json::Value response;
    response["response"] = true;
    response["id"] = json_message["id"];
    response["ok"] = true;
    response["data"] = Json::Value(Json::objectValue);

    wss_->send_websocket_frame(
        Json::writeString(Json::StreamWriterBuilder(), response));
    APP_LOG(AS_INFO) << "Sent generic success response for method: " << method;
  }
}

void RoomCaller::HandleNewConsumer(const Json::Value& data) {
  APP_LOG(AS_INFO) << "Handling new consumer request";

  std::string consumer_id = data["id"].asString();
  std::string producer_id = data["producerId"].asString();
  std::string kind = data["kind"].asString();
  uint32_t consumer_ssrc = 0;

  // Extract SSRC safely
  if (data.isMember("rtpParameters") &&
      data["rtpParameters"].isMember("encodings") &&
      data["rtpParameters"]["encodings"].isArray() &&
      data["rtpParameters"]["encodings"].size() > 0 &&
      data["rtpParameters"]["encodings"][0].isMember("ssrc")) {
    consumer_ssrc = data["rtpParameters"]["encodings"][0]["ssrc"].asUInt();
  } else {
    APP_LOG(AS_WARNING) << "Could not extract SSRC for consumer: "
                        << consumer_id;
    // Decide if you want to proceed without an SSRC. Assuming SSRC is mandatory
    // for audio. return; // Optionally exit if SSRC is critical
  }

  APP_LOG(AS_INFO) << "New consumer: ID=" << consumer_id << " Kind=" << kind
                   << " SSRC=" << consumer_ssrc
                   << " ProducerID=" << producer_id;

  // This map is likely accessed/modified by multiple threads (WebSocket thread,
  // Signaling thread) Consider adding mutex protection if not already present
  // around accesses to wss_->consumers_ For simplicity now, we assume direct
  // access is okay based on current structure, but review this.
  wss_->consumers_[consumer_id] = MediaSoupWrapper::Consumer(
      consumer_id, producer_id, data["rtpParameters"], consumer_ssrc);

  // This map is primarily used by the signaling thread when building the final
  // SDP. Posting ensures it's updated safely relative to SDP operations.
  signaling_thread()->PostTask([this, consumer_id, consumer_ssrc]() {
    if (consumer_ssrc != 0) {
      APP_LOG(AS_INFO) << "Signaling Thread: Storing SSRC " << consumer_ssrc
                       << " for consumer " << consumer_id;
      consumer_ssrc_map_[consumer_id] = consumer_ssrc;
    }
    AddAudioSinkToPeerConnection();  // Safe to call from signaling thread

    // A new consumer means the set of SSRCs (and therefore the remote SDP)
    // has changed.  Clear the flag so that MaybeSetFinalRemoteDescription()
    // will build and apply an updated SDP that includes the fresh SSRC.
    remote_description_set_.store(false);
    MaybeSetFinalRemoteDescription();
  });

  // // Add ICE candidates
  // AddIceCandidatesFromMediasoup();
  // // Set up everything else
  // SetRemoteDescriptionAndCreateAnswer(data);
  // ResumeConsumer(consumer_id);
}

void RoomCaller::HandleResponse(const Json::Value& json_message) {
  int id = json_message["id"].asInt();
  bool has_ok = json_message.isMember("ok");
  bool ok = has_ok ? json_message["ok"].asBool() : false;

  std::string method = "unknown";
  if (wss_->request_info_map_.find(id) != wss_->request_info_map_.end()) {
    method = wss_->request_info_map_[id];
  }

  APP_LOG(AS_INFO) << "Handling response for request ID: " << id
                   << ", method: " << method
                   << ", success: " << (ok ? "yes" : "no");
  APP_LOG(AS_INFO) << "Full response: "
                   << Json::writeString(Json::StreamWriterBuilder(),
                                        json_message);

  if (has_ok && !ok) {
    APP_LOG(AS_ERROR) << "Request " << id << " failed: "
                      << (json_message.isMember("errorReason")
                              ? json_message["errorReason"].asString()
                              : "Unknown error");
    return;
  }

  Json::Value data = json_message["data"];
  if (data.isNull()) {
    APP_LOG(AS_INFO) << "Response data is null for request ID: " << id;
    return;
  }

  bool state_changed = false;  // Flag to track if a relevant state changed

  if (id == wss_->rtp_caps_request_id_) {
    HandleRouterRtpCapabilities(data);
    wss_->rtp_caps_request_id_ = -1;
    state_changed = true;
  } else if (id == wss_->producer_request_id_) {
    HandleProducerTransportCreated(data);
    wss_->producer_request_id_ = -1;
    state_changed = true;
  } else if (id == wss_->consumer_request_id_) {
    HandleConsumerTransportCreated(data);
    wss_->consumer_request_id_ = -1;
    state_changed = true;
  } else if (id == wss_->producer_connect_request_id_) {
    APP_LOG(AS_INFO) << "Producer transport connected";
    wss_->producer_connect_request_id_ = -1;
    wss_->producer_transport_connected_ = true;
    state_changed = true;
    APP_LOG(AS_INFO)
        << "Creating consumer transport after producer transport connected";
    wss_->create_consumer_transport();
  } else if (id == wss_->consumer_connect_request_id_) {
    APP_LOG(AS_INFO) << "Consumer transport connected";
    wss_->consumer_connect_request_id_ = -1;
    wss_->consumer_transport_connected_ = true;
    state_changed = true;
  } else if (id == wss_->resume_consumer_request_id_) {
    APP_LOG(AS_INFO) << "Resume consumer request successful";
    wss_->resume_consumer_request_id_ = -1;

  } else if (id == wss_->join_request_id_) {
    wss_->joined_ = true;
    wss_->join_request_id_ = -1;
    state_changed = true;
    APP_LOG(AS_INFO) << "Joined room successfully";

    // Log the peers in the room
    // if (json_message.isMember("data") &&
    //     json_message["data"].isMember("peers")) {
    //   const Json::Value& peers = json_message["data"]["peers"];
    //   APP_LOG(AS_INFO) << "Room has " << peers.size() << " peers";
    // }

    // IMPORTANT: Now that we're joined, check if SDP negotiation is already
    // done
    // if (peer_connection_ &&
    //     peer_connection_->signaling_state() ==
    //         webrtc::PeerConnectionInterface::kStable &&
    //     !producer_created_) {
    //   // SDP negotiation already completed, now we can produce audio
    //   APP_LOG(AS_INFO)
    //       << "SDP already negotiated and we've joined, now producing audio";
    //   signaling_thread()->PostTask([this]() { ProduceAudio(); });
    // }

  } else if (id == wss_->produce_request_id_) {
    APP_LOG(AS_INFO) << "Produce request successful with id: "
                     << data["id"].asString();
    wss_->producer_id_ = data["id"].asString();
    producer_created_ = true;
    HandleProduceResponse(data);
    wss_->produce_request_id_ = -1;
    state_changed = true;
    return;
  } else if (id == wss_->restart_ice_request_id_) {
    APP_LOG(AS_INFO) << "Received response for restartIce request";
    HandleRestartIceResponse(data);
    wss_->restart_ice_request_id_ = -1;
    state_changed = true;
  }

  // After handling the specific response, check transport state with detailed
  // logging
  APP_LOG(AS_INFO) << "Transport states after handling response:"
                   << "\nproducer transport connected="
                   << (wss_->producer_transport_connected_ ? "true" : "false")
                   << "\nconsumer transport connected="
                   << (wss_->consumer_transport_connected_ ? "true" : "false")
                   << "\nsdp negotiation started="
                   << (sdp_negotiation_started_ ? "true" : "false")
                   << "\nlocal offer empty="
                   << (local_offer_.empty() ? "true" : "false")
                   << "\nproducer transport_id=" << wss_->producer_transport_id_
                   << "\nconsumer transport_id=" << wss_->consumer_transport_id_
                   << "\njoined=" << wss_->joined_
                   << "\nstate changed=" << state_changed
                   << "\ncreating initial_offer="
                   << creating_initial_offer.load();

  bool transports_connected = wss_->producer_transport_connected_ &&
                              wss_->consumer_transport_connected_;
  bool joined = wss_->joined_;

  // Check if we need to START SDP negotiation (existing logic)
  if (transports_connected) {
    if (!joined) {
      APP_LOG(AS_INFO) << "HandleResponse: Joining room";
      wss_->join_room();
    } else {
      // Check if we need to START SDP negotiation (existing logic)
      if (!sdp_negotiation_started_) {
        APP_LOG(AS_INFO) << "Triggering StartSdpNegotiation.";
        sdp_negotiation_started_ = true;
        creating_initial_offer.store(true);
        StartSdpNegotiation();
      }
      // Check if we need to set the final remote description
      else if (state_changed && sdp_negotiation_started_) {
        // If a relevant state (transport connect, join) just changed AND
        // negotiation has started
        APP_LOG(AS_INFO) << "HandleResponse: Relevant state changed, calling "
                            "MaybeSetFinalRemoteDescription.";
        MaybeSetFinalRemoteDescription();
      }  // state changed and sdp negotiation started
    }  // room joined
  }  // transports_connected

  // Check if both transports are connected and we're ready to start SDP
  // negotiation
  // if (wss_->producer_transport_connected_ &&
  //     wss_->consumer_transport_connected_ && !sdp_negotiation_started_) {
  //   // First, ensure we join the room if not already joined
  //   if (!wss_->joined_ && wss_->join_request_id_ == -1) {
  //     APP_LOG(AS_INFO)
  //         << "Transports connected, joining room before SDP negotiation";
  //     wss_->join_room();
  //     // Do not proceed with SDP negotiation yet - wait for join response
  //     return;
  //   }

  //   APP_LOG(AS_INFO)
  //       << "Transports connected and joined, starting SDP negotiation";

  //   sdp_negotiation_started_ = false;  // reset sdp negotiation started flag
  //   creating_initial_offer =
  //       true;  // create initial offer to get consumer ssrcs from mediasoup

  //   StartSdpNegotiation();  // Initial SDP negotiation
}

void RoomCaller::HandleRouterRtpCapabilities(const Json::Value& data) {
  APP_LOG(AS_INFO) << "Got router capabilities, creating producer transport";
  wss_->set_rtp_capabilities(data);
  wss_->create_producer_transport();
}

// Get DTLS parameters from the certificate
// This function assumes that the certificate has been initialized and is valid
Json::Value RoomCaller::GetDtlsParameters() {
  if (!certificate_) {
    APP_LOG(AS_ERROR)
        << "Cannot get DTLS parameters: no certificate initialized";
    return Json::Value(Json::objectValue);
  }

  // Get certificate fingerprints
  auto fingerprint = certificate_stats_->fingerprint;
  auto fingerprint_algorithm = certificate_stats_->fingerprint_algorithm;
  Json::Value dtAS_params;
  dtAS_params["role"] = "client";

  Json::Value fingerprints_array(Json::arrayValue);
  Json::Value fp;
  fp["algorithm"] = fingerprint_algorithm;
  fp["value"] = fingerprint;
  fingerprints_array.append(fp);
  dtAS_params["fingerprints"] = fingerprints_array;

  return dtAS_params;
}

void RoomCaller::HandleProducerTransportCreated(const Json::Value& data) {
  // Store the entire transport data first
  pending_producer_data_ = data;

  if (!data.isMember("id") || !data.isMember("iceParameters")) {
    APP_LOG(AS_ERROR) << "Missing required transport data";
    return;
  }

  // Store transport ID first
  if (wss_->producer_transport_id_.empty()) {
    wss_->producer_transport_id_ = data["id"].asString();
    APP_LOG(AS_INFO) << "Producer transport created with id: "
                     << wss_->producer_transport_id_;
  } else {
    APP_LOG(AS_WARNING)
        << "Warning: Producer transport already created with id: "
        << wss_->producer_transport_id_;
  }

  // Safely store ICE parameters
  if (data.isMember("iceCandidates") && data["iceCandidates"].isArray()) {
    APP_LOG(AS_INFO) << "Received " << data["iceCandidates"].size()
                     << " ICE candidates from mediasoup";
    producer_mediasoup_ice_candidates_.clear();  // Clear existing candidates

    for (const auto& candidate : data["iceCandidates"]) {
      producer_mediasoup_ice_candidates_.push_back(candidate);
      APP_LOG(AS_INFO) << "Mediasoup ICE candidate: "
                       << "foundation=" << candidate["foundation"].asString()
                       << " ip=" << candidate["ip"].asString()
                       << " port=" << candidate["port"].asInt()
                       << " protocol=" << candidate["protocol"].asString()
                       << " type=" << candidate["type"].asString();
    }
  }

  // Safely store ICE parameters
  if (data.isMember("iceParameters")) {
    producer_mediasoup_ice_parameters_ = data["iceParameters"];
    std::string ice_ufrag =
        producer_mediasoup_ice_parameters_["usernameFragment"].asString();
    APP_LOG(AS_INFO) << "Setting ICE credentials - ufrag: " << ice_ufrag;
  }

  // Only proceed if we have ICE and DTLS parameters from mediasoup
  if (wss_->producer_transport_id_.empty() ||
      producer_mediasoup_ice_parameters_.empty() ||
      !pending_producer_data_.isMember("dtlsParameters")) {
    APP_LOG(AS_ERROR)
        << "Missing required DTLS parameters for producer transport";
    return;
  }
  // Use the DTLS parameters provided by the server
  Json::Value dtls_params = GetDtlsParameters();
  APP_LOG(AS_INFO) << "Sending producer transport connect request";
  wss_->connect_transport(false, wss_->producer_transport_id_, dtls_params);
}

void RoomCaller::HandleConsumerTransportCreated(const Json::Value& data) {
  if (wss_->consumer_transport_id_.empty()) {
    wss_->consumer_transport_id_ = data["id"].asString();
    APP_LOG(AS_INFO) << "Consumer transport created with id: "
                     << wss_->consumer_transport_id_;
  } else {
    APP_LOG(AS_WARNING)
        << "Warning: Consumer transport already created with id: "
        << wss_->consumer_transport_id_;
  }
  pending_consumer_data_ = data;

  // Store consumer ICE candidates
  if (data.isMember("iceCandidates") && data["iceCandidates"].isArray()) {
    APP_LOG(AS_INFO) << "Received " << data["iceCandidates"].size()
                     << " consumer ICE candidates from mediasoup";
    consumer_mediasoup_ice_candidates_.clear();
    for (const auto& candidate : data["iceCandidates"]) {
      consumer_mediasoup_ice_candidates_.push_back(candidate);
      APP_LOG(AS_INFO) << "Consumer ICE candidate: "
                       << "foundation=" << candidate["foundation"].asString()
                       << " ip=" << candidate["ip"].asString()
                       << " port=" << candidate["port"].asInt()
                       << " protocol=" << candidate["protocol"].asString()
                       << " type=" << candidate["type"].asString();
    }
  }

  // Store consumer ICE parameters
  if (data.isMember("iceParameters")) {
    consumer_mediasoup_ice_parameters_ = data["iceParameters"];
    std::string ice_ufrag =
        consumer_mediasoup_ice_parameters_["usernameFragment"].asString();
    APP_LOG(AS_INFO) << "Consumer ICE credentials - ufrag: " << ice_ufrag;
  }

  // Connect consumer transport using mediasoup-provided DTLS parameters
  if (wss_->consumer_transport_id_.empty() ||
      !pending_consumer_data_.isMember("dtlsParameters")) {
    APP_LOG(AS_ERROR)
        << "Missing required DTLS parameters for consumer transport";
    return;
  }
  // Same as for the producer-side: announce the fingerprint of the certificate
  // we will actually present in the DTLS handshake.
  Json::Value dtls_params_consumer = GetDtlsParameters();
  APP_LOG(AS_INFO) << "Sending consumer transport connect request";
  wss_->connect_transport(true, wss_->consumer_transport_id_, dtls_params_consumer);
}

void RoomCaller::StartSdpNegotiation() {
  APP_LOG(AS_INFO) << "StartSdpNegotiation called";

  if (rtc::Thread::Current() != signaling_thread()) {
    APP_LOG(AS_INFO) << "Not on signaling thread, posting task";
    signaling_thread()->PostTask([this]() { StartSdpNegotiation(); });
    return;
  }

  // if (sdp_negotiation_started_) {
  //   APP_LOG(AS_INFO) << "SDP negotiation already started, skipping";
  //   return;
  // }

  if (!running_) {
    APP_LOG(AS_ERROR) << "RoomCaller not running, cannot start sdp negotiation";
    return;
  }

  if (!peer_connection_) {
    APP_LOG(AS_INFO) << "Creating peer connection";

    if (!CreatePeerConnection()) {
      APP_LOG(AS_ERROR) << "Failed to create peer connection";
      sdp_negotiation_started_ = false;
      return;
    }

    if (!peer_connection_) {
      APP_LOG(AS_ERROR) << "Peer connection is still null "
                           "after creation";
      sdp_negotiation_started_ = false;
      return;
    }

    APP_LOG(AS_INFO) << "Successfully created peer connection: "
                     << peer_connection_.get();
    // Initialize audio playout on worker thread
    if (audio_device_module_) {
      worker_thread()->PostTask([this]() {
        // Initialize both playout *and* recording so that we actually capture
        // microphone samples to send to mediasoup (otherwise we end up
        // sending silent Opus frames and consequently receive silence when
        // the server relays our own audio back).
        audio_device_module_->InitPlayout();
        audio_device_module_->StartPlayout();

        audio_device_module_->InitRecording();
        audio_device_module_->SetStereoRecording(true); // Force stereo recording
        audio_device_module_->StartRecording();

        APP_LOG(AS_INFO) << "Audio playout + recording started";
      });
    }
  } else {
    APP_LOG(AS_INFO) << "Using existing peer connection";
  }

  // Process any pending consumers
  uint32_t recv_ssrc = 0;
  if (!pending_consumers_.empty()) {
    APP_LOG(AS_INFO) << "Processing " << pending_consumers_.size()
                     << " pending consumers before creating initial offer";

    for (const auto& [consumer_id, data] : pending_consumers_) {
      APP_LOG(AS_INFO) << "Processing pending consumer: " << consumer_id;

      // Extract SSRC and store it for potentially later use (e.g., sink)
      std::string kind = data["kind"].asString();

      if (data.isMember("rtpParameters") &&
          data["rtpParameters"].isMember("encodings") &&
          !data["rtpParameters"]["encodings"].empty()) {
        recv_ssrc = data["rtpParameters"]["encodings"][0]["ssrc"].asUInt();
      }

      if (kind == "audio" && recv_ssrc != 0) {
        APP_LOG(AS_INFO) << "Storing pending AUDIO consumer SSRC: ID="
                         << consumer_id << ", SSRC=" << recv_ssrc;
        consumer_ssrc_map_[consumer_id] = recv_ssrc;
        // No need to store rtp_params here in RoomCaller
      } else {
        APP_LOG(AS_WARNING)
            << "Ignoring pending non-audio or zero-SSRC consumer: "
            << consumer_id;
      }
    }
    pending_consumers_.clear();
  }

  // if(!creating_initial_offer) {
  //  Add audio track and configure codec preferences
  cricket::AudioOptions audio_options;
  // NOTE: cricket::AudioOptions no longer contains a 'stereo' field in this
  // WebRTC revision.  Capturing remains mono; mediasoup happily accepts it
  // as long as the channel count signalled in the Opus fmtp matches (we
  // advertise two channels but the encoder will duplicate the mono signal).

  auto audio_source =
      peer_connection_factory_->CreateAudioSource(audio_options);
  if (!audio_source) {
    APP_LOG(AS_ERROR) << "Failed to create audio source";
    sdp_negotiation_started_ = false;
    return;
  }

  std::string audio_track_id = "audiotrack";
  auto audio_track = peer_connection_factory_->CreateAudioTrack(
      audio_track_id, audio_source.get());
  if (!audio_track) {
    APP_LOG(AS_ERROR) << "Failed to create audio track";
    sdp_negotiation_started_ = false;
    return;
  }

  // peer_connection_->AddTrack(audio_track, {"mediasoup"});
  webrtc::RtpTransceiverInit init;
  init.direction = webrtc::RtpTransceiverDirection::kSendRecv;

  auto result = peer_connection_->AddTransceiver(audio_track, init);
  if (!result.ok()) {
    APP_LOG(AS_ERROR) << "Failed to add audio transceiver: "
                      << result.error().message();
    sdp_negotiation_started_ = false;
    return;
  }

  // --- Force Opus codec to use payload-type 100 so that the PT in our local
  // --- offer matches the PT announced by mediasoup (and carried in all RTP
  // --- packets that the server will actually send).
  {
    webrtc::RtpTransceiverInterface* transceiver = result.value().get();
    if (transceiver) {
      auto sender_caps =
          peer_connection_factory_->GetRtpSenderCapabilities(
              cricket::MediaType::MEDIA_TYPE_AUDIO);

      std::vector<webrtc::RtpCodecCapability> opus_only;
      for (auto cap : sender_caps.codecs) {
        if (absl::EqualsIgnoreCase(cap.name, "opus")) {
          cap.preferred_payload_type = 100;  // mediasoup uses 100
          opus_only.push_back(cap);
          break;
        }
      }

      if (!opus_only.empty()) {
        webrtc::RTCError err = transceiver->SetCodecPreferences(opus_only);
        if (!err.ok()) {
          APP_LOG(AS_WARNING)
              << "Failed to set Opus codec preference (PT 100): "
              << err.message();
        } else {
          APP_LOG(AS_INFO)
              << "Set Opus codec preference with payload-type 100 for audio "
                 "transceiver";
        }
      } else {
        APP_LOG(AS_WARNING)
            << "Could not find Opus codec capability to set PT 100";
      }
    }
  }

  APP_LOG(AS_INFO) << "Successfully added audio transceiver";
  // AddAudioSinkToTrack(audio_track.get(),
  // consumer_ssrc_map_.begin()->second);

  // Add data channel
  webrtc::DataChannelInit data_channel_config;
  data_channel_config.ordered = true;
  auto data_channel = peer_connection_->CreateDataChannelOrError(
      "mediasoup", &data_channel_config);
  if (!data_channel.ok()) {
    APP_LOG(AS_WARNING) << "Failed to create data channel: "
                        << data_channel.error().message();
  } else {
    APP_LOG(AS_INFO) << "Successfully created data channel";
  }
  //} // if !creating_initial_offer

  // Configure offer-answer options
  webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
  options.offer_to_receive_audio = 1;
  options.voice_activity_detection = true;

  // Create an offer using a lambda observer
  auto create_session_observer = rtc::make_ref_counted<
      LambdaCreateSessionDescriptionObserver>(
      [this](std::unique_ptr<webrtc::SessionDescriptionInterface> desc) {
        if (!desc) {
          APP_LOG(AS_ERROR) << "Failed to create "
                               "local offer: null description";
          return;
        }

        std::string sdp;
        desc->ToString(&sdp);

        // --- mediasoup uses a fixed mapping for RTP header-extension IDs.
        // --- Ensure our offer advertises exactly the same numeric IDs so
        // --- that the packets we subsequently send are accepted by the
        // --- router.  The required mapping for *audio* is:
        // +  urn:ietf:params:rtp-hdrext:sdes:mid            -> id 1
        // +  http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time -> id 4
        // +  http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01 -> id 5
        // +  urn:ietf:params:rtp-hdrext:ssrc-audio-level    -> id 10
        auto patch_extmap = [](std::string& offer) {
          auto replace_line = [&offer](const std::string& from,
                                       const std::string& to) {
            size_t pos = offer.find(from);
            if (pos != std::string::npos)
              offer.replace(pos, from.size(), to);
          };

          replace_line("a=extmap:4 urn:ietf:params:rtp-hdrext:sdes:mid",
                       "a=extmap:1 urn:ietf:params:rtp-hdrext:sdes:mid");

          replace_line("a=extmap:1 urn:ietf:params:rtp-hdrext:ssrc-audio-level",
                       "a=extmap:10 urn:ietf:params:rtp-hdrext:ssrc-audio-level");

          replace_line("a=extmap:2 http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time",
                       "a=extmap:4 http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time");

          replace_line("a=extmap:3 http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01",
                       "a=extmap:5 http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01");

          // --- Ensure Opus is advertised on payload-type 100 (mediasoup default)
          replace_line("m=audio 9 UDP/TLS/RTP/SAVPF 111",
                       "m=audio 9 UDP/TLS/RTP/SAVPF 100");
          replace_line("a=rtpmap:111 opus/", "a=rtpmap:100 opus/");
          replace_line("a=rtcp-fb:111 ", "a=rtcp-fb:100 ");
          replace_line("a=fmtp:111 ", "a=fmtp:100 ");
        };

        patch_extmap(sdp);

        // Store patched local offer for later use
        local_offer_ = sdp;

        APP_LOG(AS_INFO)
            << "StartSdpNegotiation created (and patched) local offer successfully: " << sdp;

        // Re-create SessionDescription from the patched SDP so the
        // PeerConnection applies the corrected header-extension mapping.
        webrtc::SdpParseError parse_error;
        std::unique_ptr<webrtc::SessionDescriptionInterface> patched_desc =
            DirectCreateSessionDescription(desc->GetType(), sdp.c_str(), &parse_error);
        if (!patched_desc) {
          APP_LOG(AS_ERROR) << "Failed to parse patched SDP: " << parse_error.description;
          return;
        }

        // Set local description
        auto set_local_observer = rtc::make_ref_counted<
            LambdaSetLocalDescriptionObserver>([this](webrtc::RTCError error) {
          if (!error.ok()) {
            APP_LOG(AS_ERROR) << "Failed to set local description: " << error.message();
            return;
          }

          APP_LOG(AS_INFO)
              << "Local description set successfully, sdp negotiation started? "
              << sdp_negotiation_started_;

          // Now that we have a local offer and both transports are
          // connected, we can build and send the remote SDP
          if (!creating_initial_offer) {
            if (wss_->producer_transport_connected_ &&
                wss_->consumer_transport_connected_) {
              APP_LOG(AS_INFO)
                  << "Both transports connected, setting remote description";
              // Call SetCombinedSdp to build and set the remote SDP
              SetCombinedSdp();
            } else {
              APP_LOG(AS_INFO)
                  << "Waiting for both transports to "
                     "connect before setting remote description"
                  << " producer_connected="
                  << (wss_->producer_transport_connected_ ? "true" : "false")
                  << " consumer_connected="
                  << (wss_->consumer_transport_connected_ ? "true" : "false");
            }
          }
        });

        // Set local description with patched offer
        peer_connection_->SetLocalDescription(std::move(patched_desc), set_local_observer);
      });

  // Create the initial or final offer
  APP_LOG(AS_INFO) << "CreateOffer called with "
                   << (creating_initial_offer ? "initial" : "final")
                   << " offer";

  peer_connection_->CreateOffer(create_session_observer.get(), options);
  sdp_negotiation_started_ = true;

  // Start periodic ICE bindings if not already started
  if (!ice_binding_started_) {
    ice_binding_started_ = true;
    signaling_thread()->PostTask([this]() { this->SendPeriodicIceBindings(); });
    APP_LOG(AS_INFO) << "Started periodic ICE bindings";
  }
}

void RoomCaller::MaybeSetFinalRemoteDescription() {
  // Ensure we run on the signaling thread for PeerConnection access
  if (rtc::Thread::Current() != signaling_thread()) {
    signaling_thread()->PostTask(
        [this]() { MaybeSetFinalRemoteDescription(); });
    return;
  }

  APP_LOG(AS_INFO) << "MaybeSetFinalRemoteDescription: Checking conditions...";

  if (remote_description_set_.load()) {
    APP_LOG(AS_INFO)
        << "MaybeSetFinalRemoteDescription: Already attempted/set. Skipping.";
    return;
  }

  if (creating_initial_offer.load()) {
    APP_LOG(AS_INFO) << "MaybeSetFinalRemoteDescription: Still in initial "
                        "offer phase. Skipping.";
    return;
  }

  if (!peer_connection_) {
    APP_LOG(AS_WARNING)
        << "MaybeSetFinalRemoteDescription: No PeerConnection. Skipping.";
    return;
  }

  bool transports_ready = wss_ && wss_->producer_transport_connected_ &&
                          wss_->consumer_transport_connected_;
  bool joined = wss_ && wss_->joined_;
  bool ice_complete = peer_connection_->ice_gathering_state() ==
                      webrtc::PeerConnectionInterface::kIceGatheringComplete;
  bool correct_pc_state = peer_connection_->signaling_state() ==
                          webrtc::PeerConnectionInterface::kHaveLocalOffer;

  APP_LOG(AS_INFO) << "MaybeSetFinalRemoteDescription: Conditions:"
                   << " transports_ready="
                   << (transports_ready ? "true" : "false")
                   << " joined=" << (joined ? "true" : "false")
                   << " ice_complete=" << (ice_complete ? "true" : "false")
                   << " correct_pc_state="
                   << (correct_pc_state ? "true" : "false")
                   << " creating_initial_offer="
                   << (creating_initial_offer.load() ? "true" : "false")
                   << " remote_description_set="
                   << (remote_description_set_.load() ? "true" : "false");

  if (transports_ready && joined && ice_complete && correct_pc_state) {
    APP_LOG(AS_INFO) << "MaybeSetFinalRemoteDescription: ALL CONDITIONS MET. "
                        "Calling SetCombinedSdp.";
    // Set flag *before* calling to prevent race conditions if SetCombinedSdp
    // posts tasks
    remote_description_set_.store(true);
    SetCombinedSdp();  // This function builds and calls SetRemoteDescription
  } else {
    APP_LOG(AS_INFO)
        << "MaybeSetFinalRemoteDescription: Conditions not fully met yet.";
  }
}

std::string RoomCaller::BuildRemoteSdpBasedOnLocalOffer(
    const std::string& local_offer) {
  uint32_t consumer_ssrc =
      consumer_ssrc_map_.empty() ? 0 : consumer_ssrc_map_.begin()->second;

  // Find the consumer's RTP parameters using the ssrc
  Json::Value consumer_rtp_parameters = Json::Value();
  std::string found_consumer_id = "";
  if (wss_ && consumer_ssrc) {
    for (const auto& consumer : wss_->consumers_) {
      if (consumer.second.ssrc == consumer_ssrc) {
        consumer_rtp_parameters = consumer.second.data;
        found_consumer_id = consumer.second.id;
        APP_LOG(AS_INFO) << "Found consumer " << found_consumer_id
                         << " matching SSRC " << consumer_ssrc;
        break;
      }
    }

    if (!consumer_rtp_parameters) {
      APP_LOG(AS_ERROR) << "BuildRemoteSdpBasedOnLocalOffer: Could not find "
                           "consumer details for SSRC: "
                        << consumer_ssrc;
      return "";
    }
    if (consumer_rtp_parameters.isNull() ||
        !consumer_rtp_parameters.isMember("codecs")) {
      APP_LOG(AS_ERROR)
          << "BuildRemoteSdpBasedOnLocalOffer: Missing or invalid "
             "RTP codecs for consumer SSRC: "
          << consumer_ssrc;
      return "";
    }
  }  // if consumer_ssrc is not 0

  std::stringstream sdp;
  sdp << "v=0\r\n"
      << "o=- " << DirectCreateRandomId() << " 2 IN IP4 127.0.0.1\r\n"
      << "s=-\r\n"
      << "t=0 0\r\n"
      << "a=group:BUNDLE 0 1\r\n"
      << "a=msid-semantic: WMS *\r\n"
      << "a=ice-lite\r\n";

  // Audio (MID 0) - Use producer transport parameters
  std::string producer_ufrag =
      producer_mediasoup_ice_parameters_["usernameFragment"].asString();
  std::string producer_pwd =
      producer_mediasoup_ice_parameters_["password"].asString();
  std::string producer_algorithm =
      pending_producer_data_["dtlsParameters"]["fingerprints"][0]["algorithm"]
          .asString();
  std::string producer_fingerprint =
      pending_producer_data_["dtlsParameters"]["fingerprints"][0]["value"]
          .asString();
  std::transform(producer_algorithm.begin(), producer_algorithm.end(),
                 producer_algorithm.begin(), ::tolower);

  // -------------------------------------------------------------
  // Choose the Opus payload-type that mediasoup tells us to use.
  // The value lives in the first codec entry inside the consumer's
  // RTP parameters (e.g. "payloadType": 100).  Using a PT that
  // differs from the one mediasoup will actually send leads to all
  // RTP packets being discarded – resulting in *silence*.
  //
  // If we do not yet have consumer RTP parameters (e.g. when building
  // the very first provisional offer) fall back to the PT that
  // appears in our own local offer so that negotiation can still move
  // forward.  This branch will be corrected later, once the consumer
  // info arrives and SetCombinedSdp() is called again.
  // -------------------------------------------------------------
  int opus_pt = 111;  // sensible default

  if (!consumer_rtp_parameters.isNull() && consumer_rtp_parameters.isMember("codecs") &&
      consumer_rtp_parameters["codecs"].isArray() && !consumer_rtp_parameters["codecs"].empty()) {
    opus_pt = consumer_rtp_parameters["codecs"][0]["payloadType"].asInt();
    APP_LOG(AS_INFO) << "Using Opus PT from consumer RTP parameters: " << opus_pt;
  } else {
    // Fallback: extract the PT we used in the local offer.
    std::smatch m;
    std::regex re("a=rtpmap:(\\d+) opus/");
    std::stringstream ss(local_offer);
    std::string line;
    while (std::getline(ss, line)) {
      if (std::regex_search(line, m, re)) {
        opus_pt = std::stoi(m[1]);
        APP_LOG(AS_INFO) << "Using Opus PT from local offer: " << opus_pt;
        break;
      }
    }
  }

  sdp << "m=audio 9 UDP/TLS/RTP/SAVPF " << opus_pt << "\r\n"
      << "c=IN IP4 0.0.0.0\r\n"
      << "a=rtcp:9 IN IP4 0.0.0.0\r\n"
      << "a=ice-ufrag:" << producer_ufrag << "\r\n"
      << "a=ice-pwd:" << producer_pwd << "\r\n"
      << "a=fingerprint:" << producer_algorithm << " " << producer_fingerprint
      << "\r\n"
      << "a=setup:passive\r\n"
      << "a=mid:0\r\n"
      << "a=sendrecv\r\n"
      << "a=rtcp-mux\r\n"
      << "a=rtpmap:" << opus_pt << " opus/48000/2\r\n"
      << "a=rtcp-fb:" << opus_pt << " transport-cc\r\n"
      << "a=rtcp-fb:" << opus_pt << " nack\r\n"
      << "a=fmtp:" << opus_pt << " stereo=1;usedtx=1;useinbandfec=1\r\n"
      << "a=extmap:1 urn:ietf:params:rtp-hdrext:sdes:mid\r\n"
      << "a=extmap:4 http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time\r\n"
      << "a=extmap:5 http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01\r\n"
      << "a=extmap:10 urn:ietf:params:rtp-hdrext:ssrc-audio-level\r\n";

  std::string consumer_ufrag;
  std::string consumer_pwd;
  std::string consumer_algorithm;
  std::string consumer_fingerprint;

  if (!creating_initial_offer && consumer_ssrc) {
    sdp << "a=ssrc:" << consumer_ssrc << " cname:mediasoup\r\n";
  }

  sdp << "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"
      << "c=IN IP4 0.0.0.0\r\n";

  if (!creating_initial_offer) {
    // We intentionally do NOT advertise a fixed SSRC for the incoming audio.
    // Mediasoup may re-create the consumer at any moment (e.g. after the
    // remote peer reconnects) and pick a brand-new SSRC.  If we pin a value
    // here, the WebRTC stack will discard every packet that carries an
    // unexpected SSRC, resulting in silence.  By omitting the "a=ssrc:" line
    // we allow any SSRC and future re-subscriptions keep working.

    // Data Channel (MID 1) - Use consumer transport parameters
    consumer_ufrag =
        consumer_mediasoup_ice_parameters_["usernameFragment"].asString();
    consumer_pwd = consumer_mediasoup_ice_parameters_["password"].asString();
    consumer_algorithm =
        pending_consumer_data_["dtlsParameters"]["fingerprints"][0]["algorithm"]
            .asString();
    consumer_fingerprint =
        pending_consumer_data_["dtlsParameters"]["fingerprints"][0]["value"]
            .asString();
    std::transform(consumer_algorithm.begin(), consumer_algorithm.end(),
                   consumer_algorithm.begin(), ::tolower);
  }

  sdp << "a=setup:passive\r\n"
      << "a=mid:1\r\n"
      << "a=sctp-port:5000\r\n"
      << "a=max-message-size:262144\r\n";

  std::string sdp_str = sdp.str();
  APP_LOG(AS_INFO) << "Built remote SDP:\n" << sdp_str;
  return sdp_str;
}

void RoomCaller::SetCombinedSdp() {
  APP_LOG(AS_INFO) << "SetCombinedSdp called";
  if (rtc::Thread::Current() != signaling_thread()) {
    signaling_thread()->PostTask([this]() { SetCombinedSdp(); });
    return;
  }

  if (remote_description_set_.load()) {  // Double check flag
    APP_LOG(AS_WARNING) << "SetCombinedSdp called but remote description "
                           "already set/attempted. Ignoring.";
    return;
  }
  remote_description_set_.store(true);

  if (!peer_connection_) {
    APP_LOG(AS_ERROR) << "No peer connection available";
    return;
  }

  // Check signaling state first
  webrtc::SdpType sdp_type_to_set = webrtc::SdpType::kOffer;
  if (!creating_initial_offer &&
      peer_connection_->signaling_state() ==
          webrtc::PeerConnectionInterface::kHaveLocalOffer) {
    sdp_type_to_set = webrtc::SdpType::kAnswer;
  }

  // If we're stable and need to create a final offer
  if (!creating_initial_offer && peer_connection_->signaling_state() ==
                                     webrtc::PeerConnectionInterface::kStable) {
    APP_LOG(AS_INFO) << "Final offer with consumers "
                     << consumer_ssrc_map_.size();

    if (peer_connection_->signaling_state() !=
        webrtc::PeerConnectionInterface::kStable)
      return;  // Exit if not in the right state and not creating a new offer
  }

  if (!wss_->producer_transport_connected_ ||
      !wss_->consumer_transport_connected_) {
    APP_LOG(AS_WARNING)
        << "Not all transports connected, proceeding with offer not possible";
    return;
  }

  if (local_offer_.empty()) {
    APP_LOG(AS_ERROR) << "Local offer not set, cannot build remote sdp";
    return;
  }

  // Log SSRC information for debugging
  if (!creating_initial_offer) {
    APP_LOG(AS_INFO) << "Available ssrcs: " << consumer_ssrc_map_.size();
    for (const auto& [consumer_id, ssrc] : consumer_ssrc_map_) {
      APP_LOG(AS_INFO) << "  consumer: " << consumer_id << ", ssrc: " << ssrc;
    }
  }

  APP_LOG(AS_INFO)
      << "Setting combined SDP after producer and consumer setup, consumers? "
      << consumer_ssrc_map_.size();
  std::string remote_sdp = BuildRemoteSdpBasedOnLocalOffer(local_offer_);
  if (remote_sdp.empty()) {
    APP_LOG(AS_ERROR) << "Failed to build remote sdp";
    return;
  }

  APP_LOG(AS_INFO) << "Attempting to set Remote Description with sdp in "
                   << (creating_initial_offer ? "initial" : "final")
                   << " offer";

  webrtc::SdpParseError error;
  std::unique_ptr<webrtc::SessionDescriptionInterface> session_description =
      DirectCreateSessionDescription(sdp_type_to_set, remote_sdp.c_str(), &error);
  if (!session_description) {
    APP_LOG(AS_ERROR) << "Failed to parse sdp: " << error.description;
    return;
  }

  auto set_remote_observer = rtc::make_ref_counted<
      LambdaSetRemoteDescriptionObserver>([this](webrtc::RTCError error) {
    if (!error.ok()) {
      APP_LOG(AS_ERROR) << "SetRemoteDescription failed: " << error.message();
      return;
    }
    APP_LOG(AS_INFO) << "Remote SDP set successfully";

    // Add ICE candidates immediately after remote description is set
    APP_LOG(AS_INFO) << "Adding ICE candidates after SetRemoteDescription success";
    AddIceCandidatesFromMediasoup();

    // *** MOVE ResumeConsumer HERE ***
    // Ensure we have a valid consumer ID before resuming
    if (!consumer_ssrc_map_.empty()) {
        std::string consumer_id = consumer_ssrc_map_.begin()->first; // Assuming the first one is the relevant one
        ResumeConsumer(consumer_id);
        APP_LOG(AS_INFO) << "Requested server to resume consumer: " << consumer_id << " *after* SetRemoteDescription and AddIceCandidates";
    } else {
        APP_LOG(AS_WARNING) << "Cannot resume consumer: consumer_ssrc_map_ is empty.";
    }


    // Check if signaling is stable and all other conditions are met
    // to produce audio.
    if (peer_connection_->signaling_state() ==
        webrtc::PeerConnectionInterface::kStable) {
      APP_LOG(AS_INFO) << "Signaling stable after setting remote description.";

      // Check if we can produce audio now
      bool transports_ready = wss_ && wss_->producer_transport_connected_ &&
                              wss_->consumer_transport_connected_;
      bool joined = wss_ && wss_->joined_;

      APP_LOG(AS_INFO) << "Checking conditions for ProduceAudio: joined="
                       << (joined ? "true" : "false") << ", producer_created="
                       << (producer_created_ ? "true" : "false")
                       << ", transports_ready="
                       << (transports_ready ? "true" : "false");

      if (joined && !producer_created_ && transports_ready) {
        APP_LOG(AS_INFO) << "All conditions met after SetRemoteDescription, "
                            "calling ProduceAudio.";
        signaling_thread()->PostTask([this]() { ProduceAudio(); });
      } else {
        APP_LOG(AS_INFO) << "Conditions not yet met for ProduceAudio after "
                            "SetRemoteDescription.";
      }
    } else {
      APP_LOG(AS_INFO)
          << "Signaling state not stable yet after SetRemoteDescription";
    }
  });



  peer_connection_->SetRemoteDescription(std::move(session_description),
                                         set_remote_observer);

  APP_LOG(AS_INFO) << "SetRemoteDescription called with sdp in "
                   << (creating_initial_offer ? "initial" : "final")
                   << " offer";

  signaling_thread()->PostTask([this]() {
     std::string sdp;
     peer_connection_->remote_description()->ToString(&sdp);
     APP_LOG(AS_INFO) << "Final sdp: " << sdp; 
    });

  // This is a server interaction, safe to do directly.
  // std::string consumer_id = consumer_ssrc_map_.begin()->first;
  // ResumeConsumer(consumer_id);
  // APP_LOG(AS_INFO) << "Requested server to resume consumer: " << consumer_id;
}

std::string RoomCaller::BuildRemoteSdpOffer(const Json::Value& consumerData) {
  APP_LOG(AS_INFO) << "Building remote SDP offer for consumer "
                   << consumerData["id"].asString();

  // ------------------------------------------------------------------
  // Determine which payload-type mediasoup wants us to use for Opus.
  // Prefer the exact PT contained in the consumer's RTP parameters;
  // fall back to 111 when that field is missing.
  // ------------------------------------------------------------------
  int opus_pt = 111;
  if (consumerData.isMember("rtpParameters") &&
      consumerData["rtpParameters"].isMember("codecs") &&
      !consumerData["rtpParameters"]["codecs"].empty()) {
    opus_pt = consumerData["rtpParameters"]["codecs"][0]["payloadType"].asInt();
  }

  //const Json::Value& rtpParameters = consumerData["rtpParameters"];
  if (!consumer_mediasoup_ice_parameters_.isMember("usernameFragment") ||
      !consumer_mediasoup_ice_parameters_.isMember("password")) {
    APP_LOG(AS_ERROR) << "Missing consumer ICE parameters";
    return "";
  }

  // Get DTLS fingerprint from consumer data or fallback to producer data
  std::string algorithm, fingerprint_value;
  if (consumerData.isMember("dtlsParameters") &&
      consumerData["dtlsParameters"].isMember("fingerprints") &&
      !consumerData["dtlsParameters"]["fingerprints"].empty()) {
    const Json::Value& fingerprint =
        consumerData["dtlsParameters"]["fingerprints"][0];
    algorithm = fingerprint["algorithm"].asString();
    fingerprint_value = fingerprint["value"].asString();
    std::transform(algorithm.begin(), algorithm.end(), algorithm.begin(),
                   ::tolower);
  } else if (!pending_producer_data_["dtlsParameters"]["fingerprints"]
                  .empty()) {
    const Json::Value& fingerprint =
        pending_producer_data_["dtlsParameters"]["fingerprints"][0];
    algorithm = fingerprint["algorithm"].asString();
    fingerprint_value = fingerprint["value"].asString();
    std::transform(algorithm.begin(), algorithm.end(), algorithm.begin(),
                   ::tolower);
  } else {
    APP_LOG(AS_ERROR) << "No DTLS fingerprint available";
    return "";
  }

  std::ostringstream sdp;
  sdp << "v=0\r\n"
      << "o=- " << DirectCreateRandomUuid() << " 2 IN IP4 127.0.0.1\r\n"
      << "s=-\r\n"
      << "t=0 0\r\n"
      << "m=audio 9 UDP/TLS/RTP/SAVPF " << opus_pt << "\r\n"
      << "c=IN IP4 0.0.0.0\r\n"
      << "a=rtcp:9 IN IP4 0.0.0.0\r\n"
      << "a=ice-ufrag:"
      << consumer_mediasoup_ice_parameters_["usernameFragment"].asString()
      << "\r\n"
      << "a=ice-pwd:"
      << consumer_mediasoup_ice_parameters_["password"].asString() << "\r\n"
      << "a=fingerprint:" << algorithm << " " << fingerprint_value << "\r\n"
      << "a=setup:actpass\r\n"
      << "a=mid:0\r\n"
      << "a=sendrecv\r\n"
      << "a=rtcp-mux\r\n"
      << "a=rtcp-rsize\r\n"
      << "a=rtpmap:" << opus_pt << " opus/48000/2\r\n"
      << "a=rtcp-fb:" << opus_pt << " transport-cc\r\n"
      << "a=rtcp-fb:" << opus_pt << " nack\r\n"
      << "a=fmtp:" << opus_pt << " minptime=10;stereo=1;usedtx=1;useinbandfec=1\r\n";

  // No fixed SSRC – let the router choose.

  std::string sdp_str = sdp.str();
  APP_LOG(AS_INFO) << "Built consumer SDP offer:\n" << sdp_str;
  return sdp_str;
}

void RoomCaller::ResumeConsumer(const std::string& consumer_id) {
  // Send resume request to the server
  wss_->resume_consumer(consumer_id);
}

void RoomCaller::HandlePeerReconnection() {
  APP_LOG(AS_INFO) << "Handling peer reconnection/browser refresh";

  // Ensure we're on the signaling thread
  if (rtc::Thread::Current() != signaling_thread()) {
    signaling_thread()->PostTask([this]() { HandlePeerReconnection(); });
    return;
  }

  // Reset audio processing on worker thread
  if (audio_device_module_) {
    worker_thread()->PostTask([this]() {
      APP_LOG(AS_INFO)
          << "Resetting audio device module state for reconnection";

      // Reset ADM state
      bool was_playing = audio_device_module_->Playing();
      bool was_recording = audio_device_module_->Recording();

      if (was_playing) {
        audio_device_module_->StopPlayout();
      }

      if (was_recording) {
        audio_device_module_->StopRecording();
      }

      // Wait briefly to ensure proper reset
      rtc::Thread::Current()->SleepMs(100);

      // Reinitialize if needed
      if (!audio_device_module_->Initialized()) {
        audio_device_module_->Init();
      }

      // Restart
      if (was_playing) {
        audio_device_module_->InitPlayout();
        audio_device_module_->StartPlayout();
        APP_LOG(AS_INFO) << "Restarted audio playout";
      }

      if (was_recording) {
        audio_device_module_->InitRecording();
        audio_device_module_->SetStereoRecording(true); // Force stereo recording
        audio_device_module_->StartRecording();
        APP_LOG(AS_INFO) << "Restarted audio recording";
      }
    });
  }

  // Re-add audio sink to the peer connection
  AddAudioSinkToPeerConnection();
}

void RoomCaller::ReplaceZerosInIceCandidates(std::string& sdp) {
  if(kIgnoreZerosInIceCandidates) {
    // Replace 0.0.0.0 with 127.0.0.1 in ICE candidates
    if (sdp.find("0.0.0.0") != std::string::npos) {
      APP_LOG(AS_INFO) << "Original ICE candidate: " << sdp;
      size_t pos = sdp.find("0.0.0.0");
      while (pos != std::string::npos) {
        sdp.replace(pos, 7, "127.0.0.1");
        pos = sdp.find("0.0.0.0", pos + 9);  // 9 is length of "127.0.0.1"
      }
      APP_LOG(AS_INFO) << "Modified ICE candidate: " << sdp;
    }
  }
}

void RoomCaller::AddAudioSinkToPeerConnection() {
  APP_LOG(AS_INFO) << "AddAudioSinkToPeerConnection called";

  // Skip if we don't have any SSRCs yet
  if (consumer_ssrc_map_.empty()) {
    APP_LOG(AS_WARNING)
        << "No consumer SSRCs available yet, cannot add audio sink";
    return;
  }

  // !!! NOTE: Get the first SSRC (we only need one sink)
  uint32_t ssrc = consumer_ssrc_map_.begin()->second;

  // Find all audio tracks and add the sink
  for (const auto& receiver : peer_connection_->GetReceivers()) {
    auto track = receiver->track();
    if (!track ||
        track->kind() != webrtc::MediaStreamTrackInterface::kAudioKind) {
      continue;
    }

    std::string track_id = track->id();
    APP_LOG(AS_INFO) << "Found audio track: " << track_id
                     << ", adding sink for ssrc if not added: " << ssrc;

    // Add sink with specific SSRC criteria
    AddAudioSinkToTrack(track.get(), ssrc);
    return;  // Exit after adding to the first audio track
  }

  APP_LOG(AS_WARNING) << "No audio tracks found to attach sink";
}

// Our unified helper that both functions call
void RoomCaller::AddAudioSinkToTrack(webrtc::MediaStreamTrackInterface* track,
                                     uint32_t ssrc) {
  if (!track) {
    APP_LOG(AS_WARNING) << "Cannot add audio sink: track is null";
    return;
  }

  // Cast to AudioTrackInterface
  auto audio_track = static_cast<webrtc::AudioTrackInterface*>(track);
  if (!audio_track) {
    APP_LOG(AS_ERROR) << "Failed to cast track to AudioTrackInterface";
    return;
  }

  std::string track_id = track->id();
  APP_LOG(AS_INFO) << "Attempting to add/update audio sink for track "
                   << track_id;

  // Create the single sink instance if it doesn't exist
  if (!audio_sink_) {
    audio_sink_ = std::make_unique<AudioSink>();
    APP_LOG(AS_INFO) << "Created new global audio sink instance.";
  }

  // Always try to remove the current sink instance first.
  // This ensures we don't add the same pointer twice.
  // RemoveSink should be safe even if the sink wasn't previously added.
  if (audio_sink_) {
    APP_LOG(AS_INFO) << "Attempting to remove existing sink before adding...";
    audio_track->RemoveSink(audio_sink_.get());
  } else {
    // This case should ideally not happen if the creation logic above works
    APP_LOG(AS_WARNING) << "Audio sink pointer is null, cannot remove or add.";
    return;
  }

  // Configure the sink (optional)
  // audio_sink_->SetSsrc(ssrc); // If sink needs to filter by SSRC
  audio_sink_->SetRoomCaller(this);  // Pass RoomCaller reference if needed
  APP_LOG(AS_INFO) << "Configured audio sink for track " << track_id;

  // Now, add the sink. Since we attempted removal first, this should succeed.
  audio_track->AddSink(audio_sink_.get());

  APP_LOG(AS_INFO) << "Successfully added audio sink to track " << track_id;

  // Optional: Setup direct RTP observation if needed for debugging
  // SetupDirectRtpObservation(ssrc);

  // We don't need track_has_sink_ for the add/remove logic itself anymore
  // track_has_sink_[track_id] = true; // Can remove or keep if used elsewhere
}

void RoomCaller::AddIceCandidatesFromMediasoup() {
  APP_LOG(AS_INFO) << "Adding ICE candidates from mediasoup";

  if (!peer_connection_ || !peer_connection_->remote_description()) {
    APP_LOG(AS_ERROR) << "Cannot add ICE candidates: peer connection or remote "
                         "description missing";
    return;
  }

  // Add consumer ICE candidates (for receiving)
  for (const auto& candidate : consumer_mediasoup_ice_candidates_) {
    std::string sdp_mid = "0";  // Audio section
    int sdp_mline_index = 0;
    std::string candidate_str =
        "candidate:" + candidate["foundation"].asString() + " 1 " +
        candidate["protocol"].asString() + " " +
        std::to_string(candidate["priority"].asUInt()) + " " +
        candidate["ip"].asString() + " " +
        std::to_string(candidate["port"].asUInt()) + " typ " +
        candidate["type"].asString();
    ReplaceZerosInIceCandidates(candidate_str);

    webrtc::SdpParseError error;
    std::unique_ptr<webrtc::IceCandidateInterface> ice_candidate(
        DirectCreateIceCandidate(sdp_mid.c_str(), 
        sdp_mline_index, 
        candidate_str.c_str(),
        &error));

    if (ice_candidate) {
      peer_connection_->AddIceCandidate(ice_candidate.get());
      APP_LOG(AS_INFO) << "Added consumer ICE candidate: " << candidate_str;
    } else {
      APP_LOG(AS_ERROR) << "Failed to parse consumer ICE candidate: "
                        << error.description;
    }
  }

  // Add producer ICE candidates (for sending)
  if (producer_peer_connection_) {
    for (const auto& candidate : producer_mediasoup_ice_candidates_) {
      std::string sdp_mid = "0";  // Audio section
      int sdp_mline_index = 0;
      std::string candidate_str =
          "candidate:" + candidate["foundation"].asString() + " 1 " +
          candidate["protocol"].asString() + " " +
          std::to_string(candidate["priority"].asUInt()) + " " +
          candidate["ip"].asString() + " " +
          std::to_string(candidate["port"].asUInt()) + " typ " +
          candidate["type"].asString();
      ReplaceZerosInIceCandidates(candidate_str);

      webrtc::SdpParseError error;
      std::unique_ptr<webrtc::IceCandidateInterface> ice_candidate(
          DirectCreateIceCandidate(sdp_mid.c_str(),
                                   sdp_mline_index,
                                   candidate_str.c_str(),
                                   &error));

      if (ice_candidate) {
        producer_peer_connection_->AddIceCandidate(ice_candidate.get());
        APP_LOG(AS_INFO) << "Added producer ICE candidate: " << candidate_str;
      } else {
        APP_LOG(AS_ERROR) << "Failed to parse producer ICE candidate: "
                          << error.description;
      }
    }
  }
}

void RoomCaller::AddSingleIceCandidate(const Json::Value& candidate) {
  std::string sdp_mid = "0";  // For your single m-line
  int sdp_mline_index = 0;

  // Add TCP candidates only if UDP fails
  if (candidate["protocol"].asString() == "tcp" &&
      peer_connection_->ice_connection_state() !=
          webrtc::PeerConnectionInterface::kIceConnectionFailed) {
    return;
  }

  std::string candidate_str =
      "candidate:" + candidate["foundation"].asString() + " 1 " +
      candidate["protocol"].asString() + " " +
      std::to_string(candidate["priority"].asUInt()) + " " +
      candidate["ip"].asString() + " " +
      std::to_string(candidate["port"].asUInt()) + " typ " +
      candidate["type"].asString();

  ReplaceZerosInIceCandidates(candidate_str);
  if (candidate["protocol"].asString() == "tcp" &&
      candidate.isMember("tcptype")) {
    candidate_str += " tcptype " + candidate["tcptype"].asString();
  }

  APP_LOG(AS_INFO) << "Adding ICE candidate: " << candidate_str;

  webrtc::SdpParseError error;
  std::unique_ptr<webrtc::IceCandidateInterface> ice_candidate(
      DirectCreateIceCandidate(sdp_mid.c_str(), 
      sdp_mline_index, 
      candidate_str.c_str(),
      &error));

  if (ice_candidate) {
    peer_connection_->AddIceCandidate(ice_candidate.get());
  } else {
    APP_LOG(AS_ERROR) << "Failed to create ICE candidate: "
                      << error.description;
  }
}

void RoomCaller::ProduceAudio() {
  APP_LOG(AS_INFO) << "ProduceAudio called on thread: "
                   << rtc::Thread::Current();

  // Thread safety check
  if (rtc::Thread::Current() != signaling_thread()) {
    signaling_thread()->PostTask([this]() { ProduceAudio(); });
    return;
  }

  // Re-use the main (consumer) PeerConnection for sending microphone audio
  // so that the payload-type and MID negotiated with mediasoup match exactly
  // what we signal in the upcoming "produce" request.  Using a second,
  // independent PeerConnection with its own codec preferences caused a
  // payload-type mismatch (we sent PT=111 while the server expected PT=100)
  // and consequently no audio reached the room.

  auto* pc = peer_connection_.get();

  // State checks
  if (!pc) {
    APP_LOG(AS_ERROR) << "Cannot produce audio: producer peer connection is null";
    return;
  }

  if (!wss_->producer_transport_connected_) {
    APP_LOG(AS_ERROR)
        << "Cannot produce audio: producer transport not connected";
    return;
  }

  if (!wss_->joined_) {
    APP_LOG(AS_ERROR) << "Cannot produce audio: not joined to room";
    return;
  }

  if (producer_created_) {
    APP_LOG(AS_INFO) << "Audio producer already created, skipping";
    return;
  }

  if (pc->signaling_state() !=
      webrtc::PeerConnectionInterface::kStable) {
    APP_LOG(AS_ERROR) << "Cannot produce audio: signaling not stable";
    return;
  }

  APP_LOG(AS_INFO) << "Starting audio production. Signaling state: "
                   << webrtc::PeerConnectionInterface::AsString(
                          pc->signaling_state());

  // Make sure we have a valid audio transceiver
  auto transceivers = pc->GetTransceivers();
  std::string send_mid;
  webrtc::RtpTransceiverInterface* audio_transceiver = nullptr;

  for (auto& transceiver : transceivers) {
    if (transceiver->media_type() == cricket::MediaType::MEDIA_TYPE_AUDIO) {
      audio_transceiver = transceiver.get();
      send_mid = transceiver->mid().value_or("");
      break;
    }
  }

  // If the producer PeerConnection does NOT yet have an audio transceiver,
  // create one that is send-recv with a fresh audio track sourced from the
  // AudioDeviceModule so that we actually send microphone samples.
  if (!audio_transceiver) {
    APP_LOG(AS_INFO)
        << "Producer PC had no audio transceiver – creating one for sending";

    cricket::AudioOptions audio_options;  // use default (mono capture is fine)

    auto audio_source =
        peer_connection_factory_->CreateAudioSource(audio_options);
    if (!audio_source) {
      APP_LOG(AS_ERROR) << "Failed to create audio source for producer PC";
      return;
    }

    std::string audio_track_id = "producer_audiotrack";
    auto audio_track = peer_connection_factory_->CreateAudioTrack(
        audio_track_id, audio_source.get());
    if (!audio_track) {
      APP_LOG(AS_ERROR) << "Failed to create audio track for producer PC";
      return;
    }

    webrtc::RtpTransceiverInit init;
    init.direction = webrtc::RtpTransceiverDirection::kSendRecv;

    auto result = pc->AddTransceiver(audio_track, init);
    if (!result.ok()) {
      APP_LOG(AS_ERROR)
          << "Failed to add audio transceiver to producer PC: "
          << result.error().message();
      return;
    }

    audio_transceiver = result.value().get();
    send_mid = audio_transceiver->mid().value_or("");
  }

  if (send_mid.empty()) {
    send_mid = audio_transceiver ? audio_transceiver->mid().value_or("") : "";
  }

  if (send_mid.empty()) {
    // The MID gets chosen by WebRTC once the transceiver is negotiated.  Use
    // the well-known value "0" (first m-section) instead of the arbitrary
    // string "audio" so that the MID signalled to mediasoup matches what we
    // actually place in the RTP header extension.
    APP_LOG(AS_WARNING)
        << "MID not yet assigned; falling back to default value '0'";
    send_mid = "0";
  }

  // Set the transceiver to sendrecv
  audio_transceiver->SetDirectionWithError(
      webrtc::RtpTransceiverDirection::kSendRecv);

  // Get the sender parameters
  webrtc::RtpSenderInterface* sender = nullptr;
  auto senders = pc->GetSenders();
  for (auto& s : senders) {
    if (s->track() && s->track()->kind() == "audio") {
      sender = s.get();
      break;
    }
  }

  if (!sender) {
    APP_LOG(AS_ERROR) << "No audio sender found";
    return;
  }

  // Ensure the track is enabled
  if (sender->track()) {
    sender->track()->set_enabled(true);
    APP_LOG(AS_INFO) << "Audio track enabled for sending";
  }

  // Get the sender parameters – prefer the SSRC reported in the sender's RTP
  // parameters as that is guaranteed to be the one the stack will really use
  // on the wire once the transport becomes active.
  webrtc::RtpParameters params = sender->GetParameters();

  uint32_t ssrc = 0;
  if (!params.encodings.empty() && params.encodings[0].ssrc.has_value()) {
    ssrc = params.encodings[0].ssrc.value();
    APP_LOG(AS_INFO) << "SSRC taken from RtpParameters: " << ssrc;
  } else {
    ssrc = sender->ssrc();
    APP_LOG(AS_INFO) << "SSRC taken from sender->ssrc(): " << ssrc;
  }

  if (ssrc == 0) {
    APP_LOG(AS_ERROR) << "No valid SSRC found for audio sender";
    return;
  }

  producer_ssrc_ = ssrc; // We still store it for local reference if needed

  // Inform mediasoup of the exact SSRC we will use so that the router
  // can correctly map (and forward) the RTP stream.  Passing 0 forced the
  // server to assign a random SSRC which did not match the one actually
  // present in our outgoing packets, resulting in every audio frame being
  // discarded and therefore complete silence.  Provide the real value so
  // producer/consumer SSRCs are aligned.
  wss_->produce_audio(wss_->producer_transport_id_, ssrc /* actual SSRC */, send_mid);
}

void RoomCaller::HandleProduceResponse(const Json::Value& data) {
  if (!data.isMember("id")) {
    APP_LOG(AS_ERROR) << "Produce response missing id";
    return;
  }

  std::string producer_id = data["id"].asString();
  APP_LOG(AS_INFO) << "Producer created with id: " << producer_id;
  wss_->producer_id_ = producer_id;

  // Log our local SSRC for debugging
  auto transceivers = peer_connection_->GetTransceivers();
  for (auto& transceiver : transceivers) {
    if (transceiver->media_type() == cricket::MediaType::MEDIA_TYPE_AUDIO) {
      auto sender = transceiver->sender();
      if (sender) {
        webrtc::RtpParameters params = sender->GetParameters();
        if (!params.encodings.empty()) {
          uint32_t ssrc = params.encodings[0].ssrc.value_or(0);
          APP_LOG(AS_INFO) << "Local audio SSRC: " << ssrc;
        }
      }
      break;
    }
  }
}

void RoomCaller::HandleRestartIceResponse(const Json::Value& data) {
  APP_LOG(AS_INFO) << "Handling ICE restart response";

  if (!data.isMember("iceParameters")) {
    APP_LOG(AS_WARNING) << "Missing iceParameters in restartIce response";
    return;
  }

  // Determine which transport this restart applies to (assume producer unless
  // specified)
  bool is_consumer_restart =
      (data.isMember("transportId") &&
       data["transportId"].asString() == wss_->consumer_transport_id_);

  // Update the appropriate ICE parameters
  if (is_consumer_restart) {
    consumer_mediasoup_ice_parameters_ = data["iceParameters"];
    APP_LOG(AS_INFO) << "Updated consumer ICE parameters: "
                     << Json::writeString(Json::StreamWriterBuilder(),
                                          consumer_mediasoup_ice_parameters_);
  } else {
    producer_mediasoup_ice_parameters_ = data["iceParameters"];
    APP_LOG(AS_INFO) << "Updated producer ICE parameters: "
                     << Json::writeString(Json::StreamWriterBuilder(),
                                          producer_mediasoup_ice_parameters_);
  }

  // Update ICE candidates if provided
  if (data.isMember("iceCandidates") && data["iceCandidates"].isArray()) {
    auto& target_candidates = is_consumer_restart
                                  ? consumer_mediasoup_ice_candidates_
                                  : producer_mediasoup_ice_candidates_;
    target_candidates.clear();
    for (const auto& candidate : data["iceCandidates"]) {
      target_candidates.push_back(candidate);
      APP_LOG(AS_INFO) << (is_consumer_restart ? "Consumer" : "Producer")
                       << " ICE candidate: "
                       << candidate["foundation"].asString() << " "
                       << candidate["ip"].asString() << ":"
                       << candidate["port"].asInt();
    }
  }

  // Apply candidates immediately if peer connection exists and remote
  // description is set
  if (peer_connection_ && peer_connection_->remote_description()) {
    APP_LOG(AS_INFO) << "Applying new ICE candidates after restart";
    AddIceCandidatesFromMediasoup();
  }

  // Create a new offer with ice_restart=true
  if (peer_connection_) {
    webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
    options.ice_restart = true;

    APP_LOG(AS_INFO) << "Creating new offer with ICE restart";

    auto observer = rtc::make_ref_counted<
        LambdaCreateSessionDescriptionObserver>(
        [this](std::unique_ptr<webrtc::SessionDescriptionInterface> desc) {
          if (!desc) {
            APP_LOG(AS_ERROR) << "Failed to create offer for ICE restart";
            return;
          }

          std::string sdp;
          desc->ToString(&sdp);
          APP_LOG(AS_INFO) << "Created local offer with ICE restart: " << sdp;
          local_offer_ = sdp;

          auto set_local_observer = rtc::make_ref_counted<
              LambdaSetLocalDescriptionObserver>([this](webrtc::RTCError error) {
            if (!error.ok()) {
              APP_LOG(AS_ERROR)
                  << "Failed to set local description after ICE restart: "
                  << error.message();
              return;
            }

            APP_LOG(AS_INFO)
                << "Local description set successfully after ICE restart";

            // Build and set remote SDP
            std::string remote_sdp =
                BuildRemoteSdpBasedOnLocalOffer(local_offer_);
            if (remote_sdp.empty()) {
              APP_LOG(AS_ERROR)
                  << "Failed to build remote SDP after ICE restart";
              return;
            }

            auto set_remote_observer = rtc::make_ref_counted<
                LambdaSetRemoteDescriptionObserver>([this](webrtc::RTCError
                                                               error) {
              if (!error.ok()) {
                APP_LOG(AS_ERROR)
                    << "Failed to set remote description after ICE restart: "
                    << error.message();
                return;
              }

              APP_LOG(AS_INFO)
                  << "Remote description set successfully after ICE restart";
              AddIceCandidatesFromMediasoup();

              // Check if we need to produce audio after restart
              if (wss_->joined_ && !producer_created_) {
                APP_LOG(AS_INFO) << "Producing audio after ICE restart";
                ProduceAudio();
              }
            });

            webrtc::SdpParseError parse_error;
            std::unique_ptr<webrtc::SessionDescriptionInterface>
                session_description = DirectCreateSessionDescription(
                    webrtc::SdpType::kAnswer, remote_sdp.c_str(), &parse_error);
            if (!session_description) {
              APP_LOG(AS_ERROR)
                  << "Failed to parse remote SDP: " << parse_error.description;
              return;
            }

            peer_connection_->SetRemoteDescription(
                std::move(session_description), set_remote_observer);
          });

          peer_connection_->SetLocalDescription(std::move(desc),
                                                set_local_observer);
        });

    peer_connection_->CreateOffer(observer.get(), options);
  }
}

void RoomCaller::OnIceConnectionChange(
    webrtc::PeerConnectionInterface::IceConnectionState new_state) {
  APP_LOG(AS_INFO) << "ICE connection state changed to: "
                   << std::to_string(new_state);

  if (new_state ==
          webrtc::PeerConnectionInterface::kIceConnectionDisconnected ||
      new_state == webrtc::PeerConnectionInterface::kIceConnectionFailed) {
    if (restart_in_progress) {
      APP_LOG(AS_INFO)
          << "ICE restart already in progress, not sending another request";
      return;
    }

    if (restart_in_progress) {
      AddAudioSinkToPeerConnection();
      restart_in_progress = false;
    }

    APP_LOG(AS_WARNING)
        << "ICE connection problems detected, initiating restart";

    // Create a new offer with ice_restart = true
    if (peer_connection_) {
      webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
      options.ice_restart = true;

      auto observer =
          rtc::make_ref_counted<LambdaCreateSessionDescriptionObserver>(
              [this](
                  std::unique_ptr<webrtc::SessionDescriptionInterface> desc) {
                std::string sdp;
                desc->ToString(&sdp);
                APP_LOG(AS_INFO) << "Created offer with ICE restart: " << sdp;

                auto set_local_observer = rtc::make_ref_counted<
                    LambdaSetLocalDescriptionObserver>([this](webrtc::RTCError
                                                                  error) {
                  restart_in_progress = false;

                  if (!error.ok()) {
                    APP_LOG(AS_ERROR) << "Failed to set local description: "
                                      << error.message();
                    return;
                  }

                  APP_LOG(AS_INFO)
                      << "Local description with ICE restart set successfully";
                });

                peer_connection_->SetLocalDescription(std::move(desc),
                                                      set_local_observer);
              });

      peer_connection_->CreateOffer(observer.get(), options);
    }
  }

  switch (new_state) {
    case webrtc::PeerConnectionInterface::kIceConnectionNew:
      APP_LOG(AS_INFO) << "ICE state: New - starting negotiation";
      break;
    case webrtc::PeerConnectionInterface::kIceConnectionChecking:
      APP_LOG(AS_INFO)
          << "ICE state: Checking - performing connectivity checks";
      // Dump candidates we have
      APP_LOG(AS_INFO) << "Mediasoup ICE candidates count: "
                       << producer_mediasoup_ice_candidates_.size()
                       << " producer, "
                       << consumer_mediasoup_ice_candidates_.size()
                       << " consumer";
      break;
    case webrtc::PeerConnectionInterface::kIceConnectionConnected:
      APP_LOG(AS_INFO) << "ICE state: Connected! Media should begin flowing";
      break;
    default:
      break;
  }
}

void RoomCaller::SendPeriodicIceBindings() {
  if (!running_ || !peer_connection_)
    return;

  // Only send if we have a connection to maintain
  auto ice_state = peer_connection_->ice_connection_state();
  if (ice_state == webrtc::PeerConnectionInterface::kIceConnectionConnected ||
      ice_state == webrtc::PeerConnectionInterface::kIceConnectionCompleted) {
    APP_LOG(AS_VERBOSE) << "Sending ICE binding to keep connection alive";

    // For WebRTC, we don't need to explicitly send bindings - the ICE stack
    // will handle it. But we can use this opportunity to log connection status
    APP_LOG(AS_VERBOSE) << "ICE connection state: "
                        << webrtc::PeerConnectionInterface::AsString(ice_state)
                        << ", Signaling state: "
                        << webrtc::PeerConnectionInterface::AsString(
                               peer_connection_->signaling_state());

    // Log information about audio tracks and transceivers
    auto transceivers = peer_connection_->GetTransceivers();
    for (auto& transceiver : transceivers) {
      if (transceiver->media_type() == cricket::MediaType::MEDIA_TYPE_AUDIO) {
        auto receiver = transceiver->receiver();
        if (receiver && receiver->track()) {
          auto track = receiver->track();
          APP_LOG(AS_VERBOSE)
              << "Audio track: " << track->id()
              << ", enabled: " << (track->enabled() ? "yes" : "no");
        }
        APP_LOG(AS_VERBOSE)
            << "Audio transceiver direction: " << transceiver->direction();
      }
    }
  }

  // Schedule next binding, regardless of current state
  if (signaling_thread() && running_) {
    signaling_thread()->PostDelayedTask(
        [this]() { this->SendPeriodicIceBindings(); },
        webrtc::TimeDelta::Seconds(2));  // Every 2 seconds
  }
}

void RoomCaller::OnIceGatheringChange(
    webrtc::PeerConnectionInterface::IceGatheringState new_state) {
  APP_LOG(AS_INFO) << "ICE gathering state changed to: "
                   << std::to_string(new_state);

  if (new_state == webrtc::PeerConnectionInterface::kIceGatheringComplete) {
    bool transports_connected = wss_->producer_transport_connected_ &&
                                wss_->consumer_transport_connected_;
    if (creating_initial_offer) {
      APP_LOG(AS_INFO) << "ICE gathering complete for initial offer setup.";
      creating_initial_offer = false;  // Transition state

      // Now set the remote description with combined SDP
      if (transports_connected) {
        APP_LOG(AS_INFO) << "Setting combined remote sdp after ICE gathering";
        SetCombinedSdp();
      } else {
        APP_LOG(AS_WARNING)
            << "ICE gathering complete for initial offer, but transports not "
               "connected yet. SetCombinedSdp will be triggered later.";
      }
    } else {
      // This is not the initial offer phase.
      // We no longer call ProduceAudio here. It will be called from the
      // SetRemoteDescription callback.
      APP_LOG(AS_INFO)
          << "ICE gathering complete (not initial offer phase). ProduceAudio "
             "will be called after SetRemoteDescription succeeds.";
    }
  }
}

void RoomCaller::OnIceCandidate(
    const webrtc::IceCandidateInterface* candidate) {
  if (!candidate) {
    APP_LOG(AS_WARNING) << "OnIceCandidate called with null candidate.";
    return;
  }

  std::string sdp;
  if (!candidate->ToString(&sdp)) {
    APP_LOG(AS_ERROR) << "Failed to serialize ICE candidate to SDP string.";
  } else {
    APP_LOG(AS_INFO) << "Raw Local ICE candidate SDP: " << sdp;
  }

  // Access the internal cricket::Candidate representation
  const cricket::Candidate& internal_candidate = candidate->candidate();

  // Get the primary address associated with the candidate
  const rtc::SocketAddress& addr = internal_candidate.address();

  // Use the IceCandidateType enum for type checking
  webrtc::IceCandidateType candidate_type = internal_candidate.type();

  APP_LOG(AS_INFO) << "Parsed Local ICE Candidate Details:"
                   << "\n  sdpMid: " << candidate->sdp_mid()
                   << "\n  sdpMLineIndex: " << candidate->sdp_mline_index()
                   << "\n  Component: " << internal_candidate.component() // 1 for RTP, 2 for RTCP
                   << "\n  Protocol: " << internal_candidate.protocol()
                   << "\n  Address: " << addr.ToString() // IP:Port from candidate.address()
                   << "\n  Type: " << IceCandidateTypeToString(candidate_type) // Use helper function for enum name
                   << "\n  Priority: " << internal_candidate.priority()
                   << "\n  Network: " << internal_candidate.network_name() << ":" << internal_candidate.network_id()
                   << "\n  Foundation: " << internal_candidate.foundation();

  // Check for loopback by comparing IP address objects
  const rtc::IPAddress& ip_addr = addr.ipaddr();
  bool is_loopback = (ip_addr == rtc::IPAddress(INADDR_LOOPBACK) || // Check for 127.0.0.1
                      ip_addr == rtc::IPAddress(in6addr_loopback));  // Check for ::1

  // Specifically check if it's a loopback host candidate
  if (candidate_type == webrtc::IceCandidateType::kHost && is_loopback) {
      APP_LOG(AS_INFO) << "  -> This is a Loopback Host Candidate.";
  }
  // Optional: Check for private IPs
  // else if (candidate_type == webrtc::IceCandidateType::kHost && rtc::IPIsPrivate(ip_addr)) {
  //    APP_LOG(AS_INFO) << "  -> This is a Private IP Host Candidate.";
  // }


  // Don't send a restart request for every candidate!
  // The endless restarts are causing your authentication issues
}

void RoomCaller::OnSignalingChange(
    webrtc::PeerConnectionInterface::SignalingState new_state) {}

void RoomCaller::OnAddTrack(
    rtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver,
    const std::vector<rtc::scoped_refptr<webrtc::MediaStreamInterface>>&
        streams) {
  APP_LOG(AS_INFO) << "OnAddTrack called with receiver: " << receiver->id()
                   << " track: "
                   << (receiver->track() ? receiver->track()->id() : "null")
                   << " media_type: "
                   << cricket::MediaTypeToString(receiver->media_type())
                   << " on thread: " << rtc::Thread::Current();  // Log thread

  auto track = receiver->track();
  if (track && track->kind() == webrtc::MediaStreamTrackInterface::kAudioKind) {
    uint32_t track_ssrc = 0;
    auto params = receiver->GetParameters();
    if (!params.encodings.empty() && params.encodings[0].ssrc.has_value()) {
      track_ssrc = params.encodings[0].ssrc.value();
      APP_LOG(AS_INFO) << "OnAddTrack: Audio track SSRC from receiver parameters: " << track_ssrc;
    } else {
      APP_LOG(AS_WARNING) << "OnAddTrack: Could not get SSRC from receiver parameters for track: " << track->id();
    }

    APP_LOG(AS_INFO) << "OnAddTrack: Current consumer_ssrc_map_ contents:";
    for (const auto& [consumer_id, ssrc] : consumer_ssrc_map_) {
      APP_LOG(AS_INFO) << "  ID: " << consumer_id << ", SSRC: " << ssrc;
    }

    // Find an appropriate SSRC to potentially pass (though sink might not use
    // it) This part is tricky as the SSRC map might not be populated exactly
    // when OnAddTrack is called. For now, we might pass 0 or the first
    // available one, as the add/remove logic is the focus.
    uint32_t ssrc_to_pass = 0;
    bool found_matching_consumer = false;
    if (track_ssrc != 0) {
        for (const auto& [consumer_id, ssrc] : consumer_ssrc_map_) {
            if (ssrc == track_ssrc) {
                ssrc_to_pass = ssrc;
                found_matching_consumer = true;
                APP_LOG(AS_INFO) << "OnAddTrack: Found matching SSRC (" << ssrc << ") in consumer_ssrc_map_ for consumer_id: " << consumer_id;
                break;
            }
        }
    }
    if (!found_matching_consumer && !consumer_ssrc_map_.empty()) {
      ssrc_to_pass = consumer_ssrc_map_.begin()->second;
      APP_LOG(AS_WARNING) << "OnAddTrack: Did not find exact SSRC match for track SSRC " << track_ssrc
                          << ". Using first SSRC from map: " << ssrc_to_pass;
    } else if (!found_matching_consumer && consumer_ssrc_map_.empty()){
      APP_LOG(AS_WARNING) << "OnAddTrack: Did not find exact SSRC match for track SSRC " << track_ssrc
                          << " and consumer_ssrc_map_ is empty. Passing 0.";
    }


    APP_LOG(AS_INFO)
        << "Audio track found, calling AddAudioSinkToTrack (ssrc hint: "
        << ssrc_to_pass << ")";
    AddAudioSinkToTrack(track.get(), ssrc_to_pass);
  } else {
    APP_LOG(AS_INFO) << "Track is null or not audio, skipping sink addition.";
  }
}

void RoomCaller::OnRenegotiationNeeded() {}
void RoomCaller::OnIceConnectionReceivingChange(bool receiving) {}
void RoomCaller::OnDataChannel(
    rtc::scoped_refptr<webrtc::DataChannelInterface> channel) {
  // This function should now be called when the data channel is established
  APP_LOG(AS_INFO) << "OnDataChannel called! Label: " << channel->label()
                   << ", ID: " << channel->id() << ", State: "
                   << webrtc::DataChannelInterface::DataStateString(
                          channel->state());

  // Store the channel and register an observer to handle messages and state
  // changes
  data_channel_ = channel;  // Make sure data_channel_ is a member of RoomCaller
  data_channel_observer_ = std::make_unique<MediasoupDataChannelObserver>(
      channel);  // Store the observer
}

void RoomCaller::LogRtpStats() {
  if (!peer_connection_)
    return;

  auto receivers = peer_connection_->GetReceivers();
  for (const auto& receiver : receivers) {
    if (receiver->media_type() == cricket::MediaType::MEDIA_TYPE_AUDIO) {
      peer_connection_->GetStats(
          receiver,
          rtc::scoped_refptr<webrtc::RTCStatsCollectorCallback>(
              new rtc::RefCountedObject<StatsCallback>(
                  [](const rtc::scoped_refptr<const webrtc::RTCStatsReport>&
                         report) {
                    APP_LOG(AS_INFO) << "\nReceiver RTP Stats Report:";

                    // Iterate through the report
                    for (auto it = report->begin(); it != report->end(); ++it) {
                      const auto& stats = *it;
                      // Check if it's the inbound-rtp type
                      if (stats.type() ==
                          webrtc::RTCInboundRtpStreamStats::kType) {
                        APP_LOG(AS_INFO) << "  Type: " << stats.type()
                                         << " ID: " << stats.id();
                        // Cast to the specific type to access members safely
                        const auto* inbound_stats = &stats.cast_to<
                            const webrtc::RTCInboundRtpStreamStats>();

                        // Log specific members if they are defined
                        if (inbound_stats->ssrc.has_value())
                          APP_LOG(AS_INFO)
                              << "    SSRC: " << *inbound_stats->ssrc;
                        else
                          APP_LOG(AS_INFO) << "    SSRC: (undefined)";

                        if (inbound_stats->packets_received.has_value())
                          APP_LOG(AS_INFO) << "    Packets Received: "
                                           << *inbound_stats->packets_received;
                        else
                          APP_LOG(AS_INFO)
                              << "    Packets Received: (undefined)";

                        if (inbound_stats->bytes_received.has_value())
                          APP_LOG(AS_INFO) << "    Bytes Received: "
                                           << *inbound_stats->bytes_received;
                        else
                          APP_LOG(AS_INFO) << "    Bytes Received: (undefined)";

                        if (inbound_stats->packets_lost.has_value())
                          APP_LOG(AS_INFO) << "    Packets Lost: "
                                           << *inbound_stats->packets_lost;
                        else
                          APP_LOG(AS_INFO) << "    Packets Lost: (undefined)";

                        if (inbound_stats->codec_id.has_value())
                          APP_LOG(AS_INFO)
                              << "    Codec ID: " << *inbound_stats->codec_id;
                        else
                          APP_LOG(AS_INFO) << "    Codec ID: (undefined)";

                        if (inbound_stats->track_identifier.has_value())
                          APP_LOG(AS_INFO) << "    Track ID: "
                                           << *inbound_stats->track_identifier;
                        else
                          APP_LOG(AS_INFO) << "    Track ID: (undefined)";
                      }
                      // Optional: Log other stat types briefly if needed for
                      // context else { APP_LOG(AS_INFO) << "  Type: " <<
                      // stats.type() << " ID: " << stats.id(); }
                    }
                  })));
    }
  }
}

// Helper that reuses DirectApplication::CreatePeerConnection() logic but stores the
// result in `producer_peer_connection_` instead of the default
// `peer_connection_`.  For now we implement the minimal version that simply
// duplicates the default configuration and returns true when a connection was
// created; further fine-tuning (e.g. configuring the transceiver direction to
// send-only) happens later in ProduceAudio().
bool RoomCaller::CreateProducerPeerConnection() {
  // Already exists?
  if (producer_peer_connection_) {
    return true;
  }

  // We temporarily back up the global pointer, call the inherited method to
  // build a new PeerConnection and then restore the original.
  auto original_pc = peer_connection_;
  peer_connection_ = nullptr;
  if (!DirectApplication::CreatePeerConnection()) {
    // Failed – restore and abort
    peer_connection_ = original_pc;
    return false;
  }

  // Transfer ownership of the freshly created PeerConnection to the producer
  // slot and restore the consumer pointer.
  producer_peer_connection_ = peer_connection_;
  peer_connection_ = original_pc;

  return (producer_peer_connection_ != nullptr);
}
