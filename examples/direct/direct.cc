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
#include <algorithm>
#include <regex>
#include <atomic>
#include <map>
#include <sstream>
#include <memory>
#include <cstdlib>
#include "rtc_base/time_utils.h"
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

#include "direct.h"
#include "option.h"
#include "video.h"

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

#if LLAMA_NOTIFICATION_ENABLED
WhillatsLlama* DirectApplication::llama_ = nullptr;

void llamaCallback(bool success, const char* response, void* user_data) {
  if (success && response && *response && user_data) {
    DirectApplication* app = static_cast<DirectApplication*>(user_data);
    if(app) {
      // Safely handle the language string
      std::string language = webrtc::SpeechAudioDeviceFactory::GetLanguage();
      if (language.empty()) {
        language = "en";  // Default to English if empty
      }
      
      app->SendMessage("LLAMA:[" + language + "]" + std::string(response));
    }
  }
}
#endif

// DirectApplication Implementation
DirectApplication::DirectApplication(Options opts)
  : opts_(opts)
#if LLAMA_NOTIFICATION_ENABLED
    , llamaCallback_(llamaCallback, this)
#endif
{
  // Threads will be created in Initialize() to support full teardown/re-init
  peer_connection_factory_ = nullptr;
}

void DirectApplication::Cleanup() {
  // Explicitly close and release PeerConnection via Shutdown before stopping threads
  ShutdownInternal();

  // Remove sink before closing the connection - handle both local and remote tracks
  if (video_sink_) {
      if (video_track_) {
          RTC_LOG(LS_INFO) << "Removing video sink from local track.";
          video_track_->RemoveSink(video_sink_.get());
      }
      if (remote_video_track_) {
          RTC_LOG(LS_INFO) << "Removing video sink from remote track.";
          remote_video_track_->RemoveSink(video_sink_.get());
      }
      // Don't null the video_sink_ here to allow reuse in next connection
      RTC_LOG(LS_INFO) << "Video sink preserved for potential reuse.";
  }

  // Explicitly release PeerConnectionFactory and ADM references on the worker thread before stopping threads
  if (worker_thread()->IsCurrent()) {
      peer_connection_factory_ = nullptr;
      dependencies_.adm = nullptr;
      audio_device_module_ = nullptr;
  } else {
      worker_thread()->BlockingCall([this]() {
          peer_connection_factory_ = nullptr;
          dependencies_.adm = nullptr;
          audio_device_module_ = nullptr;
      });
  }

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

  // Log if video stream is being stopped during disconnect
  if (video_track_) {
    RTC_LOG(LS_WARNING) << "Disconnecting: Video track exists, video transmission may stop. Track enabled: " << (video_track_->enabled() ? "true" : "false");
    // Safeguard: Prevent stopping video send if not necessary
    if (video_track_->enabled() && video_source_ && video_source_->state() == webrtc::MediaSourceInterface::kLive) {
      RTC_LOG(LS_WARNING) << "Disconnecting: Video source is live and track enabled. Attempting to preserve video send state for reconnection.";
      // Note: We can't directly prevent WebRTC internal calls to SetVideoSend with null,
      // but we log to track if this is the point of stopping.
    }
  }

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
    RTC_LOG(LS_INFO) << "Closing peer connection, video transmission will stop if active.";
    peer_connection_->Close();
    peer_connection_ = nullptr;
  }
  
  // Preserve video pipeline components for reconnection but clear references
  if (remote_video_track_) {
    RTC_LOG(LS_INFO) << "Clearing remote video track reference for reconnection.";
    if (video_sink_) {
      remote_video_track_->RemoveSink(video_sink_.get());
    }
    remote_video_track_ = nullptr;
  }
  // peer_connection_factory_ = nullptr; // Keep factory alive for reconnection

  // Keep video source and sink alive for potential reconnection
  RTC_LOG(LS_INFO) << "Keeping video source and sink for potential reconnection.";

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

  // If Initialize() has not been run yet, we have no main_thread_ to post to.
  if (!main_thread_) {
    RTC_LOG(LS_WARNING) << "RunOnBackgroundThread called before Initialize – ignoring.";
    return;
  }

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

  // Create and configure WebRTC threads
  main_thread_ = rtc::Thread::CreateWithSocketServer();
  main_thread_->socketserver()->SetMessageQueue(main_thread_.get());
  DirectThreadSetName(main_thread(), "Main");
  if (!main_thread_->Start()) {
    RTC_LOG(LS_ERROR) << "Failed to start main thread";
    return false;
  }
  ws_thread_ = rtc::Thread::Create();
  worker_thread_ = rtc::Thread::Create();
  signaling_thread_ = rtc::Thread::Create();
  network_thread_ = std::make_unique<rtc::Thread>(pss());
  network_thread_->socketserver()->SetMessageQueue(network_thread_.get());

  if (!worker_thread_->Start() || !signaling_thread_->Start() ||
      !network_thread_->Start()) {
    RTC_LOG(LS_ERROR) << "Failed to start threads";
    return false;
  }

  // -------------------------------------------------------------------
  // Make sure we have a PeerConnectionFactory ready.  The previous edit
  // accidentally removed the block that created it, which led to a null
  // dereference (seg-fault) below.  For now create it with the minimal set
  // of components – this can be refined later if advanced audio features are
  // needed.
  // -------------------------------------------------------------------
  // if (!peer_connection_factory_) {
  //   dependencies_.network_thread = network_thread();
  //   dependencies_.worker_thread = worker_thread();
  //   dependencies_.signaling_thread = signaling_thread();

  //   peer_connection_factory_ = webrtc::CreatePeerConnectionFactory(
  //       dependencies_.network_thread,
  //       dependencies_.worker_thread,
  //       dependencies_.signaling_thread,
  //       /*default adm*/ nullptr,
  //       webrtc::CreateBuiltinAudioEncoderFactory(),
  //       webrtc::CreateBuiltinAudioDecoderFactory(),
  //       nullptr, nullptr, nullptr, nullptr);

  //   if (!peer_connection_factory_) {
  //     RTC_LOG(LS_ERROR) << "Failed to create PeerConnectionFactory";
  //     return false;
  //   }
  // }

  return true;
}


bool DirectApplication::CreatePeerConnection() {
  RTC_LOG(LS_INFO) << "Creating peer connection";

  if (peer_connection_) {
    peer_connection_->Close();
    peer_connection_ = nullptr;
  }

  // Log timing information to diagnose initialization order
  if (!video_source_ && opts_.video && is_caller()) {
    RTC_LOG(LS_WARNING) << "Video source not set yet for caller with video enabled. This may cause black frames. Ensure SetVideoSource is called before CreatePeerConnection.";
  } else if (video_source_) {
    RTC_LOG(LS_INFO) << "Video source state before peer connection creation: " << (video_source_->state() == webrtc::MediaSourceInterface::kLive ? "Live" : "Not Live");
    if (video_source_->state() == webrtc::MediaSourceInterface::kLive) {
      RTC_LOG(LS_INFO) << "Video source is live before peer connection creation, frames should be available.";
    } else {
      RTC_LOG(LS_WARNING) << "Video source is NOT live before peer connection creation, video may freeze.";
    }
  }

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

  webrtc::AudioDeviceModule::AudioLayer kAudioDeviceModuleType = webrtc::AudioDeviceModule::kPlatformDefaultAudio;
  // Audio device module type
#ifdef WEBRTC_SPEECH_DEVICES
  if (opts_.whisper && opts_.llama) {
    webrtc::SpeechAudioDeviceFactory::SetLlamaEnabled(true);
    webrtc::SpeechAudioDeviceFactory::SetLlamaModelFilename(opts_.llama_model);
    webrtc::SpeechAudioDeviceFactory::SetLlavaMMProjFilename(opts_.llava_mmproj);
    //llama_ = nullptr; // uncomment to send message instead of audio 
#if LLAMA_NOTIFICATION_ENABLED
    llama_ = webrtc::SpeechAudioDeviceFactory::CreateWhillatsLlama(llamaCallback_);
#endif
  } 

  if (opts_.whisper && !opts_.whisper_model.empty()) {
    kAudioDeviceModuleType = webrtc::AudioDeviceModule::kSpeechAudio; 
    webrtc::SpeechAudioDeviceFactory::SetWhisperEnabled(true);
    webrtc::SpeechAudioDeviceFactory::SetWhisperModelFilename(opts_.whisper_model);
  }

  if (opts_.llama && !opts_.llama_llava_yuv.empty()) {
    webrtc::SpeechAudioDeviceFactory::SetYuvFilename(opts_.llama_llava_yuv, 
      opts_.llama_llava_yuv_width, opts_.llama_llava_yuv_height);
  }
#endif // WEBRTC_SPEECH_DEVICES

  // Create the audio device module on the worker thread so its lifecycle runs there.
  rtc::Event adm_created;
  dependencies_.worker_thread->PostTask([this, kAudioDeviceModuleType, task_queue_factory_ptr, &adm_created]() {
    // Reset PCF and ADM references on worker thread for correct destruction.
    peer_connection_factory_ = nullptr;
    dependencies_.adm = nullptr;
    audio_device_module_ = nullptr;
    audio_device_module_ = webrtc::AudioDeviceModule::Create(
        kAudioDeviceModuleType,
        task_queue_factory_ptr);
    if (audio_device_module_) {
      RTC_LOG(LS_INFO) << "Audio device module created successfully on thread: "
                       << rtc::Thread::Current();

      // Attempt to initialize the ADM.  On head-less Linux servers PulseAudio
      // often isn't available which causes Init() to fail and WebRTC will
      // crash later when the voice engine asserts.  Detect this early and
      // transparently fall back to the dummy (no-audio) implementation so
      // that signalling and video can still work.
      int init_res = audio_device_module_->Init();
      if (init_res != 0) {
        RTC_LOG(LS_ERROR) << "Audio device module Init failed (" << init_res
                          << "), switching to DummyAudio layer";

        // Replace with dummy ADM – ignore return value, we tried our best.
        audio_device_module_ = webrtc::AudioDeviceModule::Create(
            webrtc::AudioDeviceModule::kDummyAudio,
            task_queue_factory_ptr);
        if (audio_device_module_) {
          audio_device_module_->Init();
          RTC_LOG(LS_INFO) << "Dummy audio device module created and initialized";
        } else {
          RTC_LOG(LS_ERROR) << "Failed to create Dummy audio device module";
        }
      }
    } else {
      RTC_LOG(LS_ERROR) << "Failed to create audio device module";
    }
    adm_created.Set();
  });

  // Wait for ADM creation to complete on the worker thread.
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
      // Provide template-based video encoder and decoder factories
      std::make_unique<webrtc::VideoEncoderFactoryTemplate<
          webrtc::LibvpxVp8EncoderTemplateAdapter,
          webrtc::LibvpxVp9EncoderTemplateAdapter,
          webrtc::OpenH264EncoderTemplateAdapter,
          webrtc::LibaomAv1EncoderTemplateAdapter>>(),
      std::make_unique<webrtc::VideoDecoderFactoryTemplate<
          webrtc::LibvpxVp8DecoderTemplateAdapter,
          webrtc::LibvpxVp9DecoderTemplateAdapter,
          webrtc::OpenH264DecoderTemplateAdapter,
          webrtc::Dav1dDecoderTemplateAdapter>>(),      
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

  // Create/get certificate if needed and wire it into the configuration
  if (opts_.encryption) {
      RTC_LOG(LS_INFO) << "Encryption is enabled!";
      // Re-use the already loaded certificate (created during initialization)
      // to guarantee that the fingerprint we announce in the DTLS parameters
      // is exactly the one that will be presented during the TLS handshake.
      if(!certificate_) {
        // This can happen if CreatePeerConnection() is called before we had
        // a chance to load / generate the certificate in the constructor or
        // if encryption was toggled afterwards.  Ensure we have it.
        certificate_ = DirectLoadCertificateFromEnv(opts_);
        UpdateCertificateStats();
      }

      if(certificate_) {
        config.certificates.push_back(certificate_);
      } else {
        RTC_LOG(LS_ERROR) << "Failed to obtain certificate, WebRTC DTLS would not work";
      }
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
  
  // Only increase the connection timeout modestly to avoid validation issues
  config.ice_connection_receiving_timeout = 45000;  // 45 seconds (was 30)

  cricket::ServerAddresses stun_servers;
  stun_servers.insert(rtc::SocketAddress("stun.l.google.com", 19302));
  stun_servers.insert(rtc::SocketAddress("stun1.l.google.com", 19302));
  stun_servers.insert(rtc::SocketAddress("stun2.l.google.com", 19302));
  
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

  // Use default STUN server configuration to avoid complexity

  for (const auto& server : config.servers) {
      if (server.uri.find("stun:") == 0) {
          std::string host_port = server.uri.substr(5);
          size_t colon_pos = host_port.find(':');
          if (colon_pos != std::string::npos) {
              std::string host = host_port.substr(0, colon_pos);
              int port = atoi(host_port.substr(colon_pos + 1).c_str());
              stun_servers.insert(rtc::SocketAddress(host, port));
          }
      } else if (server.uri.find("turn:") == 0 || server.uri.find("turns:") == 0) {
          std::string host_port = server.uri.substr(server.uri.find(":") + 1);
          // Strip off any query parameters
          size_t query_pos = host_port.find('?');
          if (query_pos != std::string::npos) {
              host_port = host_port.substr(0, query_pos);
          }
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
              RTC_LOG(LS_INFO) << "TURN server parsed: " << host_port << " for URI: " << server.uri;
          } else {
              RTC_LOG(LS_WARNING) << "Failed to parse TURN server port from URI: " << server.uri;
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

  // Explicitly attempt to add video track after peer connection creation if source is available
  if (video_source_) {
    RTC_LOG(LS_INFO) << "Peer connection created, attempting to add video track immediately.";
    AddVideoTrackIfSourceAvailable();
    RTC_LOG(LS_INFO) << "Post-creation video source state: " << (video_source_->state() == webrtc::MediaSourceInterface::kLive ? "Live" : "Not Live");
    if (video_source_->state() == webrtc::MediaSourceInterface::kLive) {
      RTC_LOG(LS_INFO) << "Video source is live post-peer connection creation, frames should be available for sending.";
    } else {
      RTC_LOG(LS_WARNING) << "Video source is NOT live post-peer connection creation, video may freeze or show black frames.";
    }
  } else {
    RTC_LOG(LS_WARNING) << "No video source available to add as a track after peer connection creation. Video will not be sent.";
  }

  // Additional check to ensure video send is active
  EnsureVideoSendActive();
  RTC_LOG(LS_INFO) << "Video send check performed after peer connection creation.";

  return true;
}

void DirectApplication::HandleMessage(rtc::AsyncPacketSocket* socket,
                                      const std::string& message,
                                      const rtc::SocketAddress& remote_addr) {
  RTC_LOG(LS_INFO) << "DirectApplication::HandleMessage: " << message;
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
  } else if (message.find("LLAMA:") == 0) {
    std::string payload = message.substr(6);
    std::string language, text;
    std::regex re("^\\[([^\\]]+)\\](.*)$");
    std::smatch match;
    if (std::regex_match(payload, match, re)) {
      language = match[1].str();
      text = match[2].str();
    } else {
      language = "en";
      text = payload;
    }
    RTC_LOG(LS_INFO) << "Speaking: [" << language << "] " << text;
    webrtc::SpeechAudioDeviceFactory::NotifyText(text, language);
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
        
        // Always create video sink for remote video tracks to prevent freezing
        if(!video_sink_) {
          RTC_LOG(LS_INFO) << "Initializing video sink for remote video track...";
          video_sink_ = std::make_unique<webrtc::LlamaVideoRenderer>();
          ((webrtc::LlamaVideoRenderer*)video_sink_.get())->set_is_llama(opts_.llama);
        }
        
        if (video_sink_) {
            RTC_LOG(LS_INFO) << "Attaching video sink to track: " << receiver->track()->id();
            video_track->AddOrUpdateSink(video_sink_.get(), rtc::VideoSinkWants());
            
            // Store reference to remote video track to prevent disconnection
            remote_video_track_ = video_track;
            
            RTC_LOG(LS_INFO) << "Video sink attached successfully";
            RTC_LOG(LS_INFO) << "Receiver: Video track state: " << video_track->state();
            RTC_LOG(LS_INFO) << "Receiver: Video track enabled: " << (video_track->enabled() ? "true" : "false");
        } else {
            RTC_LOG(LS_ERROR) << "Video sink is still nullptr, cannot attach to track: " << receiver->track()->id();
        }
    } else if (receiver->track()->kind() == webrtc::MediaStreamTrackInterface::kAudioKind) {
        RTC_LOG(LS_INFO) << "Audio track added.";
        // Handle audio track if needed
    }
}

void DirectApplication::OnRemoveTrack(rtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver) {
    RTC_LOG(LS_INFO) << "Track removed: " << receiver->track()->id();
    if (receiver->track()->kind() == webrtc::MediaStreamTrackInterface::kVideoKind) {
        if (video_sink_ && remote_video_track_) {
            RTC_LOG(LS_INFO) << "Removing video sink from remote track for " << (is_caller() ? "caller" : "callee");
            remote_video_track_->RemoveSink(video_sink_.get());
        }
        // Clear remote video track reference
        remote_video_track_ = nullptr;
        RTC_LOG(LS_INFO) << "Remote video track reference cleared";
    }
}

// Store an external video source (injected by host) for use when creating video tracks.
bool DirectApplication::SetVideoSource(
    rtc::scoped_refptr<webrtc::VideoTrackSourceInterface> video_source) {
  // Ensure assignment happens on the signaling thread for safety.
  if (rtc::Thread::Current() != signaling_thread()) {
    signaling_thread()->BlockingCall([this, video_source]() {
      // Safeguard: Prevent overwriting a live source with null during active connection
      if (video_source_ && video_source_->state() == webrtc::MediaSourceInterface::kLive && 
          !video_source && peer_connection_ && 
          peer_connection_->ice_connection_state() == webrtc::PeerConnectionInterface::kIceConnectionConnected) {
        RTC_LOG(LS_WARNING) << "Attempt to overwrite a live video source with null during active connection. Ignoring to prevent video freezing.";
        return;
      }
      video_source_ = video_source;
      AddVideoTrackIfSourceAvailable();
    });
  } else {
      // Safeguard: Prevent overwriting a live source with null during active connection
      if (video_source_ && video_source_->state() == webrtc::MediaSourceInterface::kLive && 
          !video_source && peer_connection_ && 
          peer_connection_->ice_connection_state() == webrtc::PeerConnectionInterface::kIceConnectionConnected) {
        RTC_LOG(LS_WARNING) << "Attempt to overwrite a live video source with null during active connection. Ignoring to prevent video freezing.";
        return true;
      }
      video_source_ = video_source;
      AddVideoTrackIfSourceAvailable();
  }

  RTC_LOG(LS_INFO) << "Video source set for " << (is_caller() ? "caller" : "callee") << ", source pointer: " << video_source_.get();
  if (!video_source_) {
    RTC_LOG(LS_WARNING) << "Warning: Video source is nullptr, no video will be sent.";
  } else {
    RTC_LOG(LS_INFO) << "Video source is set, state: " << (video_source_->state() == webrtc::MediaSourceInterface::kLive ? "Live" : "Not Live");
    if (video_source_->state() != webrtc::MediaSourceInterface::kLive) {
      RTC_LOG(LS_WARNING) << "Video source is not in live state on sender side. Black frames may be sent.";
    } else {
      RTC_LOG(LS_INFO) << "Video source is live, frames should be delivered from sender.";
    }
  }
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

// Method to add video track if source is available and peer connection exists
void DirectApplication::AddVideoTrackIfSourceAvailable() {
  if (video_source_ && peer_connection_) {
    RTC_LOG(LS_INFO) << "Adding video track to existing peer connection for " << (is_caller() ? "caller" : "callee");
    RTC_LOG(LS_INFO) << "Video source state before adding track: " << (video_source_->state() == webrtc::MediaSourceInterface::kLive ? "Live" : "Not Live");
    if (video_source_->state() != webrtc::MediaSourceInterface::kLive) {
      RTC_LOG(LS_WARNING) << "Video source is not in live state. Frames may not be delivered.";
    }
    rtc::scoped_refptr<webrtc::VideoTrackInterface> video_track(
        peer_connection_factory_->CreateVideoTrack(video_source_, "video_track"));
    video_track_ = video_track;
    video_track_->set_enabled(true);
    auto result = peer_connection_->AddTrack(video_track, {"stream1"});
    if (!result.ok()) {
      RTC_LOG(LS_ERROR) << "Failed to add video track to PeerConnection: " << result.error().message();
    } else {
      RTC_LOG(LS_INFO) << "Video track added successfully to PeerConnection";
      RTC_LOG(LS_INFO) << "Sender: Video track enabled: " << (video_track_->enabled() ? "true" : "false");
      if (video_source_->state() == webrtc::MediaSourceInterface::kLive && video_track_->enabled()) {
        RTC_LOG(LS_INFO) << "Sender: Video source is live and track is enabled. Frames should be sent to remote peer.";
      } else {
        RTC_LOG(LS_WARNING) << "Sender: Video source or track state issue. Black frames may be sent to remote peer.";
      }
      // Safeguard: Ensure video send is active with the current source
      EnsureVideoSendActive();
    }
  } else if (!peer_connection_) {
    RTC_LOG(LS_INFO) << "Peer connection not yet created, video track will be added later.";
  } else if (!video_source_) {
    RTC_LOG(LS_WARNING) << "No video source available to add as a track. Video transmission will not occur.";
  }
}

// New method to ensure video send is active with the current source
void DirectApplication::EnsureVideoSendActive() {
  if (video_track_ && video_source_ && peer_connection_) {
    RTC_LOG(LS_INFO) << "Ensuring video send is active for SSRC associated with track.";
    
    // Check if video source is live and track is enabled
    if (video_source_->state() == webrtc::MediaSourceInterface::kLive) {
      RTC_LOG(LS_INFO) << "Video source is live, ensuring it remains active for sending.";
      
      // Ensure video track is enabled
      if (!video_track_->enabled()) {
        RTC_LOG(LS_WARNING) << "Video track was disabled, re-enabling to prevent freezing.";
        video_track_->set_enabled(true);
      }
      
      // Log current connection state for debugging
      if (peer_connection_->ice_connection_state() == webrtc::PeerConnectionInterface::kIceConnectionConnected) {
        RTC_LOG(LS_INFO) << "ICE connected - video should be flowing normally.";
      }
      
    } else {
      RTC_LOG(LS_WARNING) << "Video source is not live, cannot ensure active video send.";
    }
  } else {
    RTC_LOG(LS_WARNING) << "Cannot ensure video send active: missing track (" << (video_track_ ? "present" : "missing") 
                        << "), source (" << (video_source_ ? "present" : "missing") 
                        << "), or peer connection (" << (peer_connection_ ? "present" : "missing") << ").";
  }
}
