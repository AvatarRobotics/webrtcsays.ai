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

#include "rtc_base/network.h"
#include "rtc_base/ip_address.h"
#include <unistd.h> // For usleep
#include <vector>
#include <string>
#include "rtc_base/time_utils.h"
#include "api/video_codecs/video_encoder_factory_template_libvpx_vp8_adapter.h"
#include "api/video_codecs/video_decoder_factory_template_libvpx_vp8_adapter.h"

#include "direct.h"
#include "option.h"

// String split from option.cc
std::vector<std::string> stringSplit(std::string input, std::string delimiter);

void DirectApplication::rtcInitialize() {
  rtc::InitializeSSL();
}

void DirectApplication::rtcCleanup() {
  rtc::CleanupSSL();
}

rtc::IPAddress IPFromString(absl::string_view str) {
  rtc::IPAddress ip;
  RTC_CHECK(rtc::IPFromString(str, &ip));
  return ip;
}

// DirectApplication Implementation
DirectApplication::DirectApplication(Options opts)
  : opts_(opts)
{

  main_thread_ = rtc::Thread::CreateWithSocketServer();
  main_thread_->socketserver()->SetMessageQueue(main_thread_.get());
  DirectThreadSetName(main_thread(), "Main");

  ws_thread_ = rtc::Thread::Create();
  worker_thread_ = rtc::Thread::Create();
  signaling_thread_ = rtc::Thread::Create();
  network_thread_ = std::make_unique<rtc::Thread>(DirectApplication::pss());
  network_thread_->socketserver()->SetMessageQueue(network_thread_.get());

  peer_connection_factory_ = nullptr;
}

void DirectApplication::Cleanup() {
  // Explicitly close and release PeerConnection via Shutdown before stopping threads
  ShutdownInternal();

  // Remove sink before closing the connection
  if (video_track_ && video_sink_) {
      RTC_LOG(LS_INFO) << "Removing video sink.";
      // Assuming video_track_ is accessed/modified only on signaling thread after creation
      video_track_->RemoveSink(video_sink_.get());
      video_sink_ = nullptr;
  }

  // Reset factory on signaling thread before stopping threads
  if (signaling_thread()->IsCurrent()) { // Avoid blocking call if already on signaling thread
       if (peer_connection_factory_) {
           peer_connection_factory_ = nullptr;
       }
  } else {
      signaling_thread()->BlockingCall([this]() {
          if (peer_connection_factory_) {
              peer_connection_factory_ = nullptr;
          }
      });
  }

  // Explicitly release the ADM reference on the worker thread before stopping threads
  if (worker_thread()->IsCurrent()) { // Avoid blocking call if already on worker thread
      dependencies_.adm = nullptr; 
      audio_device_module_ = nullptr; // Also release the direct member ref
  } else {
     worker_thread()->BlockingCall([this]() {
         dependencies_.adm = nullptr;
         audio_device_module_ = nullptr; // Also release the direct member ref
     });
  }

  // Remove the task that resets PC on the network thread
  // network_thread_->PostTask([this]() {
  //   peer_connection_ = nullptr; 
  //   peer_connection_factory_ = nullptr;
  // });

  if (rtc::Thread::Current() != main_thread_.get()) {
    main_thread_->PostTask([this]() { Cleanup(); });
    return;
  }

  // Stop threads in reverse order with a small delay to ensure graceful shutdown
  if (network_thread_) {
    network_thread_->Quit();
    network_thread_->Stop();
    rtc::Thread::SleepMs(50); // Small delay (50ms) to allow pending operations to complete
    network_thread_.reset();
  }
  if (worker_thread_) {
    worker_thread_->Quit();
    worker_thread_->Stop();
    rtc::Thread::SleepMs(50);
    worker_thread_.reset();
  }
  if (signaling_thread_) {
    signaling_thread_->Quit();
    signaling_thread_->Stop();
    rtc::Thread::SleepMs(50);
    signaling_thread_.reset();
  }
  if (ws_thread_) {
    ws_thread_->Quit();
    ws_thread_->Stop();
    rtc::Thread::SleepMs(50);
    ws_thread_.reset();
  }
  if (main_thread_) {
    main_thread_.reset();
  }

  // Clear remaining members
  socket_factory_.reset();
  
  // Close all tracked sockets to ensure they are removed from PhysicalSocketServer
  RTC_LOG(LS_INFO) << "Closing tracked sockets";
  for (auto* socket : tracked_sockets_) {
    RTC_LOG(LS_INFO) << "Closing socket";
    if (socket) {
      RTC_LOG(LS_INFO) << "Socket is not null, closing";
      socket->Close();
    }
  }
  tracked_sockets_.clear();
  // Add a longer delay to ensure any pending socket operations are completed
  rtc::Thread::SleepMs(500); // Longer delay (500ms) to allow pending operations to complete

  if(pss_) {
    pss_->WakeUp();
    delete pss_;
  }
  if(network_manager_) {
    delete network_manager_;
  }

}

void DirectApplication::Disconnect() {
  if (rtc::Thread::Current() != main_thread_.get()) {
    main_thread_->PostTask([this]() { Disconnect(); });
    return;
  }

  // Close active connections and reset connection state without destroying core resources
  RTC_LOG(LS_INFO) << "Disconnecting active connections";

  // Close all tracked sockets to ensure they are removed from PhysicalSocketServer
  RTC_LOG(LS_INFO) << "Closing tracked sockets during disconnect";
  for (auto* socket : tracked_sockets_) {
    if (socket) {
      RTC_LOG(LS_INFO) << "Closing socket during disconnect";
      socket->Close();
    }
  }
  tracked_sockets_.clear();

  // Reset connection-specific state
  if (peer_connection_) {
    peer_connection_->Close();
    peer_connection_ = nullptr;
  }
  // peer_connection_factory_ = nullptr; // Keep factory alive for reconnection

  // Reset message sequence counters for potential reconnection
  ice_candidates_sent_ = 0;
  ice_candidates_received_ = 0;
  sdp_fragments_sent_ = 0;
  sdp_fragments_received_ = 0;

  // Small delay to allow pending operations to complete
  rtc::Thread::SleepMs(100); // Delay (100ms) to ensure operations complete

  RTC_LOG(LS_INFO) << "Disconnected, ready for reconnection";
}

void DirectApplication::Run() {
  RTC_DCHECK_RUN_ON(&sequence_checker_);

  rtc::Event quit_event;

  // Set up quit handler on network thread
  network_thread_->PostTask([this, &quit_event]() {
    while (!should_quit_) {
      rtc::Thread::Current()->ProcessMessages(100);
    }
    quit_event.Set();
  });

  // Process messages on main thread until network thread signals quit
  while (!quit_event.Wait(webrtc::TimeDelta::Millis(0))) {
    rtc::Thread::Current()->ProcessMessages(100);
  }

  // Final cleanup only if quitting completely
  if (should_quit_) {
    Cleanup();
  } else {
    Disconnect(); // Otherwise just disconnect for potential reconnection
  }
}

void DirectApplication::RunOnBackgroundThread() {
  RTC_DCHECK(!rtc::Thread::Current()); // Ensure not on a WebRTC thread yet

  // Use main_thread_ instead of creating a new thread to avoid conflicts
  main_thread_->PostTask([this]() {
    while (!should_quit_) {
      main_thread_->ProcessMessages(100); // Process messages with 100ms timeout
    }
    if (should_quit_) {
      Cleanup();
    } else {
      Disconnect(); // Otherwise just disconnect for potential reconnection
    }
  });
}

DirectApplication::~DirectApplication() {
  if (!should_quit_)
    should_quit_ = true;
  Cleanup();
}

bool DirectApplication::Initialize() {
  RTC_DCHECK_RUN_ON(&sequence_checker_);

  if (!worker_thread_->Start() || !signaling_thread_->Start() ||
      !network_thread_->Start()) {
    RTC_LOG(LS_ERROR) << "Failed to start threads";
    return false;
  }
  return true;
}

bool DirectApplication::CreatePeerConnection() {

  RTC_LOG(LS_INFO) << "Creating peer connection";

  if (peer_connection_) {
      peer_connection_->Close();
      peer_connection_ = nullptr;
    }
    peer_connection_factory_ = nullptr;

  // Create/get certificate if needed
  if (opts_.encryption && !certificate_) {
    certificate_ = DirectLoadCertificateFromEnv(opts_);
    certificate_stats_ = certificate_->GetSSLCertificate().GetStats();
    RTC_LOG(LS_INFO) << "Using certificate with fingerprint: "
                      << certificate_stats_->fingerprint << " and algorithm: "
                      << certificate_stats_->fingerprint_algorithm;
  }

  // Create a single task queue factory that will be used consistently
  std::unique_ptr<webrtc::TaskQueueFactory> task_queue_factory = 
      webrtc::CreateDefaultTaskQueueFactory();
  
  // Store the raw pointer for logging
  webrtc::TaskQueueFactory* task_queue_factory_ptr = task_queue_factory.get();
  
  // Create audio task queue - this will be used for audio operations
  audio_task_queue_ = task_queue_factory->CreateTaskQueue(
      "AudioTaskQueue", webrtc::TaskQueueFactory::Priority::NORMAL);
  
  RTC_LOG(LS_INFO) << "Created TaskQueueFactory: " << task_queue_factory_ptr
                  << " and AudioTaskQueue: " << audio_task_queue_.get();

  // Task queue factory setup
  dependencies_.network_thread = network_thread();
  dependencies_.worker_thread = worker_thread();
  dependencies_.signaling_thread = signaling_thread();

  // Audio device module type
  webrtc::AudioDeviceModule::AudioLayer kAudioDeviceModuleType = webrtc::AudioDeviceModule::kPlatformDefaultAudio;
#ifdef WEBRTC_SPEECH_DEVICES
  if (opts_.whisper) {
    kAudioDeviceModuleType = webrtc::AudioDeviceModule::kSpeechAudio;
    webrtc::SpeechAudioDeviceFactory::SetWhisperModelFilename(opts_.whisper_model);
    webrtc::SpeechAudioDeviceFactory::SetLlamaModelFilename(opts_.llama_model);
  }
#endif // WEBRTC_SPEECH_DEVICES

  rtc::Event adm_created;
  audio_device_module_ = nullptr;
  dependencies_.worker_thread->PostTask([this, kAudioDeviceModuleType, task_queue_factory_ptr, &adm_created]() {
      audio_device_module_ = webrtc::AudioDeviceModule::Create(
        kAudioDeviceModuleType,
        task_queue_factory_ptr
      );
      if (audio_device_module_) {
          RTC_LOG(LS_INFO) << "Audio device module created successfully on thread: " << rtc::Thread::Current();
          // No need to call Init() here; CreatePeerConnectionFactory will do it
      } else {
          RTC_LOG(LS_ERROR) << "Failed to create audio device module";
      }
      adm_created.Set();
  });

  // Wait for ADM creation to complete
  adm_created.Wait(webrtc::TimeDelta::Seconds(1));
  if (!audio_device_module_) {
      RTC_LOG(LS_ERROR) << "Audio device module creation failed after task execution";
      return false;
  }

  dependencies_.adm = audio_device_module_;
  dependencies_.task_queue_factory = std::move(task_queue_factory);

  // PeerConnectionFactory creation
  peer_connection_factory_ = webrtc::CreatePeerConnectionFactory(
      dependencies_.network_thread,
      dependencies_.worker_thread,
      dependencies_.signaling_thread,
      dependencies_.adm,
      webrtc::CreateBuiltinAudioEncoderFactory(),
      webrtc::CreateBuiltinAudioDecoderFactory(),
      // Pass nullptr for video factories to use internal defaults
      opts_.video ? \
      std::make_unique<webrtc::VideoEncoderFactoryTemplate<webrtc::OpenH264EncoderTemplateAdapter>>() : nullptr,
      opts_.video ? \
      std::make_unique<webrtc::VideoDecoderFactoryTemplate<webrtc::OpenH264DecoderTemplateAdapter>>(): nullptr, // video_encoder_factory
      nullptr, // audio_mixer
      nullptr  // audio_processing
  );

  if (!peer_connection_factory_) {
    RTC_LOG(LS_ERROR) << "Failed to create PeerConnectionFactory";
    return false; // Handle error appropriately
  }

  webrtc::PeerConnectionInterface::RTCConfiguration config;
  config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
  webrtc::PeerConnectionFactory::Options options = {};
  if(opts_.encryption) {
      RTC_LOG(LS_INFO) << "Encryption is enabled!";
      auto certificate = DirectLoadCertificateFromEnv(opts_);
      config.certificates.push_back(certificate);
  } else {
      // WARNING! FOLLOWING CODE IS FOR DEBUG ONLY!
      options.disable_encryption = true;
      peer_connection_factory_->SetOptions(options);
      // END OF WARNING
  }

  config.type = webrtc::PeerConnectionInterface::IceTransportsType::kAll;
  // Only set essential ICE configs
  config.bundle_policy = webrtc::PeerConnectionInterface::kBundlePolicyMaxBundle;
  config.rtcp_mux_policy = webrtc::PeerConnectionInterface::kRtcpMuxPolicyRequire;
  
  // Simple, reliable intervals
  config.ice_connection_receiving_timeout = 30000;  // 30 seconds instead of default 15
  config.ice_backup_candidate_pair_ping_interval = 2500; // Keep backup pairs alive

  cricket::ServerAddresses stun_servers;
  std::vector<cricket::RelayServerConfig> turn_servers;

  if(opts_.turns.size()) {
   std::vector<std::string> turnsParams = stringSplit(opts_.turns, ",");
   if(turnsParams.size() == 3) {
      webrtc::PeerConnectionInterface::IceServer iceServer;
      iceServer.uri = turnsParams[0];
      iceServer.username = turnsParams[1];
      iceServer.password = turnsParams[2];
      config.servers.push_back(iceServer);
    }
  }

  webrtc::PeerConnectionInterface::IceServer stun_server;
  stun_server.uri = "stun:stun.l.google.com:19302";
  config.servers.push_back(stun_server);

  for (const auto& server : config.servers) {
      if (server.uri.find("stun:") == 0) {
          std::string host_port = server.uri.substr(5);
          size_t colon_pos = host_port.find(':');
          if (colon_pos != std::string::npos) {
              std::string host = host_port.substr(0, colon_pos);
              int port = atoi(host_port.substr(colon_pos + 1).c_str());
              stun_servers.insert(rtc::SocketAddress(host, port));
          }
      } else if (server.uri.find("turn:") == 0) {
          std::string host_port = server.uri.substr(5);
          size_t colon_pos = host_port.find(':');
          if (colon_pos != std::string::npos) {
              cricket::RelayServerConfig turn_config;
              turn_config.credentials = cricket::RelayCredentials(server.username, server.password);
              turn_config.ports.push_back(cricket::ProtocolAddress(
                  rtc::SocketAddress(
                      host_port.substr(0, colon_pos),
                      atoi(host_port.substr(colon_pos + 1).c_str())),
                  cricket::PROTO_UDP));
              turn_servers.push_back(turn_config);
          }
      }
  }

  RTC_LOG(LS_INFO) << "Configured STUN/TURN servers:";
  for (const auto& addr : stun_servers) {
      RTC_LOG(LS_INFO) << "  STUN Server: " << addr.ToString();
  }
  for (const auto& turn : turn_servers) {
      for (const auto& addr : turn.ports) {
          RTC_LOG(LS_INFO) << "  TURN Server: " << addr.address.ToString()
                            << " (Protocol: " << addr.proto << ")";
      }
  }

  // Ensure packet socket factory exists (used for port allocator later)
  if (!socket_factory_) {
    socket_factory_ = std::make_unique<rtc::BasicPacketSocketFactory>(pss());
  }
  // Recreate network manager if needed, passing the PhysicalSocketServer (which is a SocketFactory)
  if (!network_manager_) {
    // Pass pss() which returns the PhysicalSocketServer*, a valid SocketFactory*
    network_manager_ = new rtc::BasicNetworkManager(pss());
  }

  // Add VPN list to ignore
  if(vpns_.size()) {
    for(auto vpn : vpns_) {
      std::string vpn_ip = vpn;
      rtc::NetworkMask vpn_mask(IPFromString(vpn_ip), 32);
      network_manager_->set_vpn_list({vpn_mask});
      network_manager_->set_network_ignore_list({vpn_ip});
      network_manager_->StartUpdating();
    }
  }

  auto port_allocator = std::make_unique<cricket::BasicPortAllocator>(
      network_manager_, socket_factory_.get());
  RTC_DCHECK(port_allocator.get());    

  port_allocator->SetConfiguration(
      stun_servers,
      turn_servers,
      0,  // Keep this as 0
      webrtc::PeerConnectionInterface::ContinualGatheringPolicy::GATHER_CONTINUALLY,
      nullptr,
      {}
  );

  // Allow flexible port allocation for UDP
  uint32_t flags = 0;
  flags |= cricket::PORTALLOCATOR_ENABLE_ANY_ADDRESS_PORTS; 

  port_allocator->set_flags(flags);
  port_allocator->set_step_delay(cricket::kMinimumStepDelay);  // Speed up gathering
  port_allocator->set_candidate_filter(cricket::CF_ALL);  // Allow all candidate types

  webrtc::PeerConnectionDependencies pc_dependencies(this);
  pc_dependencies.allocator = std::move(port_allocator);

  auto pcf_result = peer_connection_factory_->CreatePeerConnectionOrError(
      config, std::move(pc_dependencies));
  RTC_DCHECK(pcf_result.ok());    
  peer_connection_ = pcf_result.MoveValue();
  RTC_LOG(LS_INFO) << "PeerConnection created successfully.";

  return true;
}

void DirectApplication::HandleMessage(rtc::AsyncPacketSocket* socket,
                                      const std::string& message,
                                      const rtc::SocketAddress& remote_addr) {
  if (message.find("ICE:") == 0) {
    ice_candidates_received_++;
    SendMessage("ICE_ACK:" + std::to_string(ice_candidates_received_));

    // Send our ICE candidate if we haven't sent all
    if (ice_candidates_sent_ < kMaxIceCandidates) {
      ice_candidates_sent_++;
      SendMessage("ICE:" + std::to_string(ice_candidates_sent_));
    }
    // Start SDP exchange when ICE is complete
    else if (ice_candidates_received_ >= kMaxIceCandidates &&
             sdp_fragments_sent_ == 0) {
      sdp_fragments_sent_++;
      SendMessage("SDP:" + std::to_string(sdp_fragments_sent_));
    }
  } else if (message.find("SDP:") == 0) {
    sdp_fragments_received_++;
    SendMessage("SDP_ACK:" + std::to_string(sdp_fragments_received_));

    // Send our SDP fragment if we haven't sent all
    if (sdp_fragments_sent_ < kMaxSdpFragments) {
      sdp_fragments_sent_++;
      SendMessage("SDP:" + std::to_string(sdp_fragments_sent_));
    }
    // Send BYE when all exchanges are complete
    else if (sdp_fragments_received_ >= kMaxSdpFragments &&
             ice_candidates_received_ >= kMaxIceCandidates) {
      SendMessage("BYE");
    }
  }
}

bool DirectApplication::SendMessage(const std::string& message) {
  if (!tcp_socket_) {
    RTC_LOG(LS_ERROR) << "Cannot send message, socket is null";
    return false;
  }
  RTC_LOG(LS_INFO) << "Sending message: " << message;
  size_t sent = tcp_socket_->Send(message.c_str(), message.length(),
                                  rtc::PacketOptions());
  if (sent <= 0) {
    RTC_LOG(LS_ERROR) << "Failed to send message, error: " << tcp_socket_->GetError();
    return false;
  }
  RTC_LOG(LS_INFO) << "Successfully sent " << sent << " bytes";
  return true;
}

rtc::Socket* DirectApplication::WrapSocket(int s) {
  rtc::Socket* socket = DirectApplication::pss()->WrapSocket(s);
  if (socket) {
    tracked_sockets_.push_back(socket);
  }
  return socket;
}

rtc::Socket* DirectApplication::CreateSocket(int family, int type) {
  rtc::Socket* socket = DirectApplication::pss()->CreateSocket(family, type);
  if (socket) {
    tracked_sockets_.push_back(socket);
  }
  return socket;
}

void DirectApplication::OnAddTrack(rtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver,
                           const std::vector<rtc::scoped_refptr<webrtc::MediaStreamInterface>>& streams) {
    RTC_LOG(LS_INFO) << "Track added: " << receiver->track()->id() << " Kind: " << receiver->track()->kind();

    if (receiver->track()->kind() == webrtc::MediaStreamTrackInterface::kVideoKind) {
        RTC_LOG(LS_INFO) << "Video track added for " << (is_caller() ? "caller" : "callee");
        auto* video_track = static_cast<webrtc::VideoTrackInterface*>(receiver->track().get());
        
        // Ensure we don't add multiple sinks
        if(!video_sink_) {
            video_sink_ = std::make_unique<ConsoleVideoRenderer>();
        }
        
        if (video_sink_) {
            video_track->AddOrUpdateSink(video_sink_.get(), rtc::VideoSinkWants());
        }
    } else if (receiver->track()->kind() == webrtc::MediaStreamTrackInterface::kAudioKind) {
        RTC_LOG(LS_INFO) << "Audio track added.";
        // Handle audio track if needed
    }
}

void DirectApplication::OnRemoveTrack(rtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver) {
    RTC_LOG(LS_INFO) << "Track removed: " << receiver->track()->id();
    if (receiver->track()->kind() == webrtc::MediaStreamTrackInterface::kVideoKind) {
        auto* video_track = static_cast<webrtc::VideoTrackInterface*>(receiver->track().get());
        if (video_sink_) {
            RTC_LOG(LS_INFO) << "Removing video track for " << (is_caller() ? "caller" : "callee");
            video_track->RemoveSink(video_sink_.get());
            video_sink_ = nullptr;
        }
    }
}

// Store an external video source (injected by host) for use when creating video tracks.
bool DirectApplication::SetVideoSource(
    rtc::scoped_refptr<webrtc::VideoTrackSourceInterface> video_source) {
  // Ensure assignment happens on the signaling thread for safety.
  if (rtc::Thread::Current() != signaling_thread()) {
    signaling_thread()->BlockingCall([this, video_source]() {
      video_source_ = video_source;
    });
  } else {
      video_source_ = video_source;
  }

  RTC_LOG(LS_INFO) << "Video source set for " << (is_caller() ? "caller" : "callee");
  return true;
}

bool DirectApplication::SetVideoSink(
    std::unique_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> video_sink) {
  // Ensure assignment happens on the signaling thread for safety.
  if (rtc::Thread::Current() != signaling_thread()) {
    signaling_thread()->BlockingCall([this, &video_sink]() {
      video_sink_ = std::move(video_sink);
    });
  } else {
      video_sink_ = std::move(video_sink);
  }

  RTC_LOG(LS_INFO) << "Video sink set for " << (is_caller() ? "caller" : "callee");
  return true;
}
