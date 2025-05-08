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

#include "direct.h"
#include "option.h"

#include <json/json.h>
#include "call/rtp_stream_receiver_controller.h"

class MediaSoupWrapper;  // Forward declaration
class AudioSink;
class MediasoupDataChannelObserver;

class DIRECT_API RoomCaller : public DirectApplication {
 public:
  explicit RoomCaller(Options opts);
  ~RoomCaller();
  
  void Run();
  bool Connect();
  void Start();
  void HandleNotification(const Json::Value& json_message);
  void HandleResponse(const Json::Value& json_message);
 protected:
  void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState new_state) override;
  void OnAddTrack(rtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver,
      const std::vector<rtc::scoped_refptr<webrtc::MediaStreamInterface>>&streams) override;  
  void OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState new_state) override;
  void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState new_state) override;
  void OnIceCandidate(const webrtc::IceCandidateInterface* candidate) override;
  void OnRenegotiationNeeded() override;
  void OnIceConnectionReceivingChange(bool receiving) override; 
  void OnDataChannel(rtc::scoped_refptr<webrtc::DataChannelInterface> channel) override;

  // From DirectApplication
  virtual void HandleMessage(rtc::AsyncPacketSocket* socket,
                             const std::string& message,
                             const rtc::SocketAddress& remote_addr) override {}

  virtual bool SendMessage(const std::string& message) override { return false; }

 private:
  Options opts_;
  void Shutdown() {} // TBD
  void OnWebSocketMessage(const std::string& message);
  void HandleRequest(const Json::Value& json_message);
  void HandleNewConsumer(const Json::Value& json_message);
  void HandleRouterRtpCapabilities(const Json::Value& data);
  void HandleProducerTransportCreated(const Json::Value& data);
  void HandleConsumerTransportCreated(const Json::Value& data);
  void HandleProduceResponse(const Json::Value& data);
  void HandleRestartIceResponse(const Json::Value& data);
  void StartSdpNegotiation();
  void MaybeSetFinalRemoteDescription();
  void SetCombinedSdp();
  void AddIceCandidatesFromMediasoup();
  void AddAudioSinkToPeerConnection();
  void AddAudioSinkToTrack(webrtc::MediaStreamTrackInterface* track, uint32_t ssrc);  
  void AddSingleIceCandidate(const Json::Value& candidate);
  void ReplaceZerosInIceCandidates(std::string& sdp);
  std::string BuildRemoteSdpBasedOnLocalOffer(const std::string& local_offer);
  void ProduceAudio();
  Json::Value GetDtlsParameters();
  std::string BuildRemoteSdpOffer(const Json::Value& consumerData);
  void ResumeConsumer(const std::string& consumer_id);
  void SendPendingRequests();
  void ReconnectWebSocket();
  void HandlePeerReconnection();
  virtual bool SetVideoSource(rtc::scoped_refptr<webrtc::VideoTrackSourceInterface> video_source) override { return false; }
  virtual bool SetVideoSink(std::unique_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> video_sink) override { return false; }
  std::unique_ptr<MediaSoupWrapper> wss_;
  std::unique_ptr<AudioSink> audio_sink_;
  std::map<std::string, bool> track_has_sink_;

  // std::unique_ptr<MediaPacketObserver> media_packet_observer_;
  // rtc::Thread* demuxer_thread_ = nullptr;
  // std::unique_ptr<webrtc::RtpDemuxer> rtp_demuxer_;
  // std::mutex demuxer_mutex_;
  // void RegisterRtpPacketSink(uint32_t ssrc, webrtc::RtpPacketSinkInterface* sink);
  // rtc::Event demuxer_initialized_event_;
  // std::atomic<bool> demuxer_initialized_ = false;

  Json::Value pending_producer_data_;  // Renamed for clarity
  Json::Value pending_consumer_data_;  // Renamed for clarity
  std::vector<std::string> pending_ice_candidates_;
  std::vector<Json::Value> producer_mediasoup_ice_candidates_;
  Json::Value producer_mediasoup_ice_parameters_;
  std::vector<Json::Value> consumer_mediasoup_ice_candidates_;
  Json::Value consumer_mediasoup_ice_parameters_;
  std::map<std::string, uint32_t> consumer_ssrc_map_;
  uint32_t producer_ssrc_ = 0;
  std::map<std::string, Json::Value> pending_consumers_;
  std::string local_offer_;

  std::atomic<bool> creating_initial_offer{false}; // Assuming you have this
  std::atomic<bool> remote_description_set_{false}; 

  rtc::scoped_refptr<webrtc::DataChannelInterface> data_channel_;
  std::unique_ptr<MediasoupDataChannelObserver> data_channel_observer_;
  
  bool producer_created_ = false;
  bool sdp_negotiation_started_ = false;
  
  bool restart_in_progress = false;

  void SendPeriodicIceBindings();
  bool ice_binding_started_ = false;

  static constexpr bool kIgnoreZerosInIceCandidates = false;
  static constexpr int kMaxReconnectAttempts = 5;
  static constexpr int kReconnectDelayMs = 1000;
  int reconnect_attempts_ = 0;

  bool running_ = false;
  void KeepAlive();
public:
  void LogRtpStats();
};
