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

#include "direct.h"
#include "option.h"

rtc::IPAddress IPFromString(absl::string_view str) {
  rtc::IPAddress ip;
  RTC_CHECK(rtc::IPFromString(str, &ip));
  return ip;
}

// Function to create a self-signed certificate
rtc::scoped_refptr<rtc::RTCCertificate> CreateCertificate() {
  auto key_params = rtc::KeyParams::RSA(2048);  // Use RSA with 2048-bit key
  auto identity = rtc::SSLIdentity::Create("webrtc", key_params);
  if (!identity) {
    RTC_LOG(LS_ERROR) << "Failed to create SSL identity";
    return nullptr;
  }
  return rtc::RTCCertificate::Create(std::move(identity));
}

// Function to read a file into a string
std::string ReadFile(const std::string& path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    RTC_LOG(LS_ERROR) << "Failed to open file: " << path;
    return "";
  }
  std::ostringstream oss;
  oss << file.rdbuf();
  return oss.str();
}

// Function to load a certificate from PEM files
rtc::scoped_refptr<rtc::RTCCertificate> LoadCertificate(
    const std::string& cert_path,
    const std::string& key_path) {
  // Read the certificate and key files
  std::string cert_pem = ReadFile(cert_path);
  std::string key_pem = ReadFile(key_path);

  if (cert_pem.empty() || key_pem.empty()) {
    RTC_LOG(LS_ERROR) << "Failed to read certificate or key file";
    return nullptr;
  }

  // Log the PEM strings for debugging
  RTC_LOG(LS_VERBOSE) << "Certificate PEM:\n" << cert_pem;
  RTC_LOG(LS_VERBOSE) << "Private Key PEM:\n" << key_pem;

  // Create an SSL identity from the PEM strings
  auto identity = rtc::SSLIdentity::CreateFromPEMStrings(key_pem, cert_pem);
  if (!identity) {
    RTC_LOG(LS_ERROR) << "Failed to create SSL identity from PEM strings";
    return nullptr;
  }

  return rtc::RTCCertificate::Create(std::move(identity));
}

// Function to load certificate from environment variables or fall back to
// CreateCertificate
rtc::scoped_refptr<rtc::RTCCertificate> LoadCertificateFromEnv(Options opts) {
  // Get paths from environment variables
  const char* cert_path = opts.webrtc_cert_path.empty()
                              ? std::getenv("WEBRTC_CERT_PATH")
                              : opts.webrtc_cert_path.c_str();
  const char* key_path = opts.webrtc_key_path.empty()
                             ? std::getenv("WEBRTC_KEY_PATH")
                             : opts.webrtc_key_path.c_str();

  if (cert_path && key_path) {
    RTC_LOG(LS_INFO) << "Loading certificate from " << cert_path << " and "
                     << key_path;
    auto certificate = LoadCertificate(cert_path, key_path);
    if (certificate) {
      return certificate;
    }
    RTC_LOG(LS_WARNING) << "Failed to load certificate from files; falling "
                           "back to CreateCertificate";
  } else {
    RTC_LOG(LS_WARNING)
        << "Environment variables WEBRTC_CERT_PATH and WEBRTC_KEY_PATH not "
           "set; falling back to CreateCertificate";
  }

  // Fall back to CreateCertificate
  return CreateCertificate();
}

// DirectApplication Implementation
DirectApplication::DirectApplication() {
  pss_ = std::make_unique<rtc::PhysicalSocketServer>();

  main_thread_ = rtc::Thread::CreateWithSocketServer();
  main_thread_->socketserver()->SetMessageQueue(main_thread_.get());
  main_thread_->SetName("Main", nullptr);
  main_thread_->WrapCurrent();

  worker_thread_ = rtc::Thread::Create();
  signaling_thread_ = rtc::Thread::Create();
  network_thread_ = std::make_unique<rtc::Thread>(pss_.get());
  network_thread_->socketserver()->SetMessageQueue(network_thread_.get());

  peer_connection_factory_ = nullptr;
}

void DirectApplication::CleanupSocketServer() {
  if (rtc::Thread::Current() != main_thread_.get()) {
    main_thread_->PostTask([this]() { CleanupSocketServer(); });
    return;
  }

  // Stop threads in reverse order
  if (network_thread_) {
    network_thread_->Stop();
    network_thread_.reset();
  }
  if (worker_thread_) {
    worker_thread_->Stop();
    worker_thread_.reset();
  }
  if (signaling_thread_) {
    signaling_thread_->Stop();
    signaling_thread_.reset();
  }
  if (main_thread_) {
    main_thread_->UnwrapCurrent();
    main_thread_.reset();
  }

  // Clear remaining members
  network_manager_.reset();
  socket_factory_.reset();
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

  // Final cleanup
  CleanupSocketServer();
  
}

DirectApplication::~DirectApplication() {
  CleanupSocketServer();
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

bool DirectApplication::CreatePeerConnection(Options opts_) {

  RTC_LOG(LS_INFO) << "Creating peer connection";

  if (peer_connection_) {
      peer_connection_->Close();
      peer_connection_ = nullptr;
    }
    peer_connection_factory_ = nullptr;

  // Create/get certificate if needed
  if (opts_.encryption && !certificate_) {
    certificate_ = LoadCertificateFromEnv(opts_);
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
      opts_.video ? std::make_unique<webrtc::VideoEncoderFactoryTemplate<
          webrtc::LibvpxVp8EncoderTemplateAdapter,
          webrtc::LibvpxVp9EncoderTemplateAdapter,
          webrtc::OpenH264EncoderTemplateAdapter,
          webrtc::LibaomAv1EncoderTemplateAdapter>>() : nullptr,
      opts_.video ? std::make_unique<webrtc::VideoDecoderFactoryTemplate<
          webrtc::LibvpxVp8DecoderTemplateAdapter,
          webrtc::LibvpxVp9DecoderTemplateAdapter,
          webrtc::OpenH264DecoderTemplateAdapter,
          webrtc::Dav1dDecoderTemplateAdapter>>() : nullptr,
      nullptr, nullptr);

  webrtc::PeerConnectionInterface::RTCConfiguration config;
  config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
  webrtc::PeerConnectionFactory::Options options = {};
  if(opts_.encryption) {
      RTC_LOG(LS_INFO) << "Encryption is enabled!";
      auto certificate = LoadCertificateFromEnv(opts_);
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
  config.ice_check_min_interval = 200;             // More frequent checks
  config.ice_check_interval_strong_connectivity = 1000;  // Check every second when connected
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
              int port = std::stoi(host_port.substr(colon_pos + 1));
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
                      std::stoi(host_port.substr(colon_pos + 1))),
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
    network_manager_ = std::make_unique<rtc::BasicNetworkManager>(pss());
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
      network_manager_.get(), socket_factory_.get());
  RTC_DCHECK(port_allocator.get());    

  port_allocator->SetConfiguration(
      stun_servers,
      turn_servers,
      0,  // Keep this as 0
      webrtc::PeerConnectionInterface::ContinualGatheringPolicy::GATHER_CONTINUALLY,
      nullptr,
      std::nullopt
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
    RTC_LOG(LS_ERROR) << "Failed to send message, error: " << errno;
    return false;
  }
  RTC_LOG(LS_INFO) << "Successfully sent " << sent << " bytes";
  return true;
}


