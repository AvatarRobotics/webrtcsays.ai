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

#ifndef WEBRTC_DIRECT_DIRECT_H_
#define WEBRTC_DIRECT_DIRECT_H_

#include <future>
#include <memory>
#include <optional>
#include <string>

#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/create_peerconnection_factory.h"
#include "api/environment/environment_factory.h"
#include "api/jsep.h"
#include "api/peer_connection_interface.h"
#include "api/task_queue/default_task_queue_factory.h"
#include "api/video_codecs/video_decoder_factory_template.h"
#include "api/video_codecs/video_decoder_factory_template_dav1d_adapter.h"
#include "api/video_codecs/video_decoder_factory_template_libvpx_vp8_adapter.h"
#include "api/video_codecs/video_decoder_factory_template_libvpx_vp9_adapter.h"
#include "api/video_codecs/video_decoder_factory_template_open_h264_adapter.h"
#include "api/video_codecs/video_encoder_factory_template.h"
#include "api/video_codecs/video_encoder_factory_template_libaom_av1_adapter.h"
#include "api/video_codecs/video_encoder_factory_template_libvpx_vp8_adapter.h"
#include "api/video_codecs/video_encoder_factory_template_libvpx_vp9_adapter.h"
#include "api/video_codecs/video_encoder_factory_template_open_h264_adapter.h"
#include "modules/audio_device/audio_device_impl.h"
#include "p2p/base/basic_packet_socket_factory.h"
#include "p2p/base/port_allocator.h"
#include "p2p/client/basic_port_allocator.h"
#include "pc/peer_connection.h"
#include "pc/peer_connection_factory.h"
#include "pc/test/mock_peer_connection_observers.h"
#include "rtc_base/async_tcp_socket.h"
#include "rtc_base/logging.h"
#include "rtc_base/openssl_identity.h"
#include "rtc_base/physical_socket_server.h"
#include "rtc_base/ref_counted_object.h"
#include "rtc_base/socket_address.h"
#include "rtc_base/ssl_adapter.h"
#include "rtc_base/ssl_identity.h"
#include "rtc_base/thread.h"
#include "rtc_base/virtual_socket_server.h"
#include "rtc_base/third_party/sigslot/sigslot.h"
#include "system_wrappers/include/clock.h"
#include "system_wrappers/include/field_trial.h"
#include "option.h"

#ifdef WEBRTC_SPEECH_DEVICES
#include "modules/audio_device/speech/speech_audio_device_factory.h"
#endif

#include "rtc_base/system/rtc_export.h"

class LambdaCreateSessionDescriptionObserver
    : public webrtc::CreateSessionDescriptionObserver {
 public:
  explicit LambdaCreateSessionDescriptionObserver(
      std::function<void(std::unique_ptr<webrtc::SessionDescriptionInterface>
                             desc)> on_success)
      : on_success_(on_success) {}
  void OnSuccess(webrtc::SessionDescriptionInterface* desc) override {
    // Takes ownership of answer, according to CreateSessionDescriptionObserver
    // convention.
    on_success_(absl::WrapUnique(desc));
  }
  void OnFailure(webrtc::RTCError error) override {
    RTC_LOG(LS_ERROR) << "CreateOffer failed: " << error.message();
  }

 private:
  std::function<void(std::unique_ptr<webrtc::SessionDescriptionInterface> desc)>
      on_success_;
};

class LambdaSetLocalDescriptionObserver
    : public webrtc::SetLocalDescriptionObserverInterface {
 public:
  explicit LambdaSetLocalDescriptionObserver(
      std::function<void(webrtc::RTCError)> on_complete)
      : on_complete_(on_complete) {}
  void OnSetLocalDescriptionComplete(webrtc::RTCError error) override {
    on_complete_(error);
  }

 private:
  std::function<void(webrtc::RTCError)> on_complete_;
};

class LambdaSetRemoteDescriptionObserver
    : public webrtc::SetRemoteDescriptionObserverInterface {
 public:
  explicit LambdaSetRemoteDescriptionObserver(
      std::function<void(webrtc::RTCError)> on_complete)
      : on_complete_(on_complete) {}
  void OnSetRemoteDescriptionComplete(webrtc::RTCError error) override {
    on_complete_(error);
  }

 private:
  std::function<void(webrtc::RTCError)> on_complete_;
};

// Function to create a self-signed certificate
rtc::scoped_refptr<rtc::RTCCertificate> CreateCertificate();

// Function to load a certificate from PEM files
rtc::scoped_refptr<rtc::RTCCertificate> LoadCertificate(const std::string& cert_path, const std::string& key_path);

// Function to load certificate from environment variables or fall back to CreateCertificate
rtc::scoped_refptr<rtc::RTCCertificate> LoadCertificateFromEnv(Options opts);



class __attribute__((visibility("default"))) DirectApplication : public webrtc::PeerConnectionObserver {
 public:
  DirectApplication();
  virtual ~DirectApplication();

  // Initialize threads and basic WebRTC infrastructure
  bool Initialize();
  bool CreatePeerConnection(Options opts);

  // Run the application event loop
  void Run();
  rtc::PhysicalSocketServer* pss() { return pss_.get(); }

  static void __attribute__((visibility("default"))) rtcInitializeSSL();
  static void __attribute__((visibility("default"))) rtcCleanupSSL();

 protected:
    
  // Thread getters for derived classes
  rtc::Thread* signaling_thread() { return signaling_thread_.get(); }
  rtc::Thread* worker_thread() { return worker_thread_.get(); }
  rtc::Thread* network_thread() { return network_thread_.get(); }
  rtc::Thread* main_thread() { return main_thread_.get(); }
  webrtc::PeerConnectionFactoryDependencies dependencies_ = {};
  rtc::scoped_refptr<webrtc::AudioDeviceModule> audio_device_module_;
  std::unique_ptr<webrtc::TaskQueueBase, webrtc::TaskQueueDeleter> audio_task_queue_;
  rtc::scoped_refptr<webrtc::PeerConnectionInterface> peer_connection_;
  rtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface>
      peer_connection_factory_;
  std::unique_ptr<rtc::BasicNetworkManager> network_manager_;
  std::unique_ptr<rtc::BasicPacketSocketFactory> socket_factory_;

  rtc::scoped_refptr<rtc::RTCCertificate> certificate_;  // Store certificate
  std::unique_ptr<rtc::SSLCertificateStats> certificate_stats_;
  rtc::SSLCertificateStats* certificate_stats() const {
    return certificate_stats_.get();
  }
  rtc::RTCCertificate* certificate() const { return certificate_.get(); }

  void CleanupSocketServer();

  void QuitThreads() {
    should_quit_ = true;  // Add this member to DirectApplication class
    if (network_thread_)
      network_thread_->Quit();
    if (worker_thread_)
      worker_thread_->Quit();
    if (signaling_thread_)
      signaling_thread_->Quit();
    if (main_thread_)
      main_thread_->Quit();
  }

  // Common message handling
  virtual void HandleMessage(rtc::AsyncPacketSocket* socket,
                             const std::string& message,
                             const rtc::SocketAddress& remote_addr);

  virtual bool SendMessage(const std::string& message);

  // Message sequence tracking
  int ice_candidates_sent_ = 0;
  int ice_candidates_received_ = 0;
  int sdp_fragments_sent_ = 0;
  int sdp_fragments_received_ = 0;
  static constexpr int kMaxIceCandidates = 3;
  static constexpr int kMaxSdpFragments = 2;

  std::unique_ptr<rtc::AsyncTCPSocket> tcp_socket_;

  std::atomic<bool> should_quit_{false};

  static constexpr int kDebugNoEncryptionMode = true;
 private:
  // std::unique_ptr<rtc::VirtualSocketServer> vss_;
  std::unique_ptr<rtc::Thread> main_thread_;

  // WebRTC threads
  std::unique_ptr<rtc::Thread> signaling_thread_;
  std::unique_ptr<rtc::Thread> worker_thread_;
  std::unique_ptr<rtc::Thread> network_thread_;

  // Ensure methods are called on correct thread
  webrtc::SequenceChecker sequence_checker_;
  std::unique_ptr<rtc::PhysicalSocketServer> pss_;

  // VPN addresses
  std::vector<std::string> vpns_;
};

class __attribute__((visibility("default"))) DirectPeer : public DirectApplication {
 public:
  DirectPeer(Options opts);
  ~DirectPeer() override;

  void Start();

  // Override DirectApplication methods
  virtual void HandleMessage(rtc::AsyncPacketSocket* socket,
                             const std::string& message,
                             const rtc::SocketAddress& remote_addr) override;

  virtual bool SendMessage(const std::string& message) override;

  // PeerConnectionObserver implementation
  void OnSignalingChange(
      webrtc::PeerConnectionInterface::SignalingState new_state) override;
  void OnAddTrack(
      rtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver,
      const std::vector<rtc::scoped_refptr<webrtc::MediaStreamInterface>>&
          streams) override;
  void OnRemoveTrack(
      rtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver) override;
  void OnDataChannel(
      rtc::scoped_refptr<webrtc::DataChannelInterface> channel) override;
  void OnRenegotiationNeeded() override;
  void OnIceConnectionChange(
      webrtc::PeerConnectionInterface::IceConnectionState new_state) override;
  void OnIceGatheringChange(
      webrtc::PeerConnectionInterface::IceGatheringState new_state) override;
  void OnIceCandidate(const webrtc::IceCandidateInterface* candidate) override;
  void OnIceConnectionReceivingChange(bool receiving) override;

 protected:
  void Shutdown();
  bool is_caller() const { return opts_.mode == "caller"; }
  webrtc::PeerConnectionInterface* peer_connection() const {
    return peer_connection_.get();
  }

  // Session description methods
  void SetRemoteDescription(const std::string& sdp);
  void AddIceCandidate(const std::string& candidate_sdp);

 private:
  Options opts_;  // Store command line options
  std::vector<std::string> pending_ice_candidates_;

  rtc::scoped_refptr<LambdaCreateSessionDescriptionObserver>
      create_session_observer_;
  rtc::scoped_refptr<LambdaSetLocalDescriptionObserver>
      set_local_description_observer_;
  rtc::scoped_refptr<LambdaSetRemoteDescriptionObserver>
      set_remote_description_observer_;
};

class __attribute__((visibility("default"))) DirectCallee : public DirectPeer, public sigslot::has_slots<> {
 public:
  explicit DirectCallee(Options opts);
  ~DirectCallee() override;

  // Start listening for incoming connections
  bool StartListening();

 private:
  void OnNewConnection(rtc::AsyncListenSocket* socket,
                       rtc::AsyncPacketSocket* new_socket);
  void OnMessage(rtc::AsyncPacketSocket* socket,
                 const unsigned char* data,
                 size_t len,
                 const rtc::SocketAddress& remote_addr);

  int local_port_;
  // std::unique_ptr<rtc::AsyncTCPSocket> tcp_socket_;
  std::unique_ptr<rtc::AsyncTcpListenSocket>
      listen_socket_;  // Changed to unique_ptr
};

class RTC_EXPORT __attribute__((visibility("default"))) DirectCaller : public DirectPeer, public sigslot::has_slots<> {
 public:
  explicit DirectCaller(Options opts);
  ~DirectCaller() override;

  // Connect and send messages
  bool Connect();
  // bool SendMessage(const std::string& message);

 private:
  // Called when data is received on the socket
  void OnMessage(rtc::AsyncPacketSocket* socket,
                 const unsigned char* data,
                 size_t len,
                 const rtc::SocketAddress& remote_addr);

  // Called when connection is established
  void OnConnect(rtc::AsyncPacketSocket* socket);

  rtc::SocketAddress remote_addr_;
  // std::unique_ptr<rtc::AsyncTCPSocket> tcp_socket_;
};

#endif  // WEBRTC_DIRECT_DIRECT_H_
