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

#include <string>
#include <vector>

#include "api/peer_connection_interface.h"
#include "api/rtc_event_log/rtc_event_log.h"
#include "api/task_queue/default_task_queue_factory.h"
#include "media/engine/internal_decoder_factory.h"
#include "media/engine/internal_encoder_factory.h"
#include "modules/audio_device/include/audio_device.h"
#include "modules/audio_processing/include/audio_processing.h"
#include "modules/video_capture/video_capture_factory.h"
#include "pc/video_track_source.h"
#include "rtc_base/ref_counted_object.h"
#include "api/video/video_frame.h"
#include "rtc_base/logging.h"

#include "direct.h"
#include "video.h"

DirectPeer::DirectPeer(
    Options opts) 
  : DirectApplication(opts)
{
}

DirectPeer::~DirectPeer() {
  // Shutdown should have been called, but as a safety net:
  if (peer_connection_) {
      ShutdownInternal(); // Ensure cleanup happens
  }
}

void DirectPeer::ShutdownInternal() {
    RTC_LOG(LS_INFO) << "Shutting down peer connection (DirectPeer::ShutdownInternal)";

    // Perform close and release on the signaling thread
    signaling_thread()->BlockingCall([this]() {
        RTC_DCHECK_RUN_ON(signaling_thread());
        // Release tracks first so their destructors can safely invoke into WebRTC threads
        audio_track_ = nullptr;
        video_track_ = nullptr;
        video_source_ = nullptr;
        // Now close the PeerConnection, after tracks are released
        if (peer_connection_) {
            peer_connection_->Close();
            peer_connection_ = nullptr; // Release ref ptr on signaling thread
        }
    });

    // Do not reset peer_connection_factory_ to allow reconnection
    RTC_LOG(LS_INFO) << "Peer connection closed and released on signaling thread.";
}

void DirectPeer::Start() {

  // Reset the closed event before starting a new connection attempt
  ResetConnectionClosedEvent();
  
  // Set a timeout to automatically fail the connection if it takes too long
  main_thread()->PostDelayedTask([this]() {
    if (connection_closed_event_.Wait(webrtc::TimeDelta::Zero())) {
      // Connection already closed, nothing to do
      return;
    }
    
    // Check if we're still in a connecting state after timeout
    if (peer_connection_ && 
        peer_connection_->ice_connection_state() != webrtc::PeerConnectionInterface::kIceConnectionConnected &&
        peer_connection_->ice_connection_state() != webrtc::PeerConnectionInterface::kIceConnectionCompleted) {
      RTC_LOG(LS_WARNING) << "Connection timeout after 30 seconds. Current ICE state: " 
                          << static_cast<int>(peer_connection_->ice_connection_state());
      connection_closed_event_.Set(); // Signal timeout
    }
  }, webrtc::TimeDelta::Seconds(30)); // 30 second timeout

  signaling_thread()->PostTask([this]() {

    if(peer_connection_ == nullptr) {
        if (!CreatePeerConnection()) {
            RTC_LOG(LS_ERROR) << "Failed to create peer connection";
            return;
        }
    }

    if (is_caller()) {
        cricket::AudioOptions audio_options;

        auto audio_source = peer_connection_factory_->CreateAudioSource(audio_options);
        RTC_DCHECK(audio_source.get());
        audio_track_ = peer_connection_factory_->CreateAudioTrack("audio_track", audio_source.get());
        RTC_DCHECK(audio_track_);

        webrtc::RtpTransceiverInit ainit;
        ainit.direction = webrtc::RtpTransceiverDirection::kSendRecv;
        auto at_result = peer_connection_->AddTransceiver(audio_track_, ainit);
        RTC_DCHECK(at_result.ok());
        auto atransceiver = at_result.value();

        // Force the direction immediately after creation
        auto adirection_result = atransceiver->SetDirectionWithError(webrtc::RtpTransceiverDirection::kSendRecv);
        RTC_LOG(LS_INFO) << "Initial audio transceiver direction set for " << (is_caller() ? "caller" : "callee")
            << ", result:" << (adirection_result.ok() ? "success" : "failed");
    
        // if video_source_ is not nullptr, create a video track
        if (opts_.video) {
            if (!video_source_) {
                // Create a static periodic video source (local fake source)
                auto static_source = rtc::make_ref_counted<webrtc::StaticPeriodicVideoTrackSource>(false);
                if (opts_.llama && !opts_.llama_llava_yuv.empty()) {
                    // Load custom YUV data if available
                    RTC_LOG(LS_INFO) << "Loading YUV data from " << opts_.llama_llava_yuv;
                    YUVData &yuv = webrtc::SpeechAudioDeviceFactory::GetYuvData();
                    if(yuv.width > 0 && yuv.height > 0) {
                        static_source->static_periodic_source().LoadYuvData(yuv);
                    } else {
                        RTC_LOG(LS_ERROR) << "Invalid YUV data";
                    }
                }
                video_source_ = static_source;
                RTC_LOG(LS_INFO) << "Created StaticPeriodicVideoTrackSource";
            }

            video_track_ = peer_connection_factory_->CreateVideoTrack(video_source_, "video_track");
            RTC_DCHECK(video_track_);

            webrtc::RtpTransceiverInit vinit;
            vinit.direction = webrtc::RtpTransceiverDirection::kSendRecv;
            auto vt_result = peer_connection_->AddTransceiver(video_track_, vinit);
            RTC_DCHECK(vt_result.ok());
            auto vtransceiver = vt_result.value();

            // Force the direction immediately after creation
            auto vdirection_result = vtransceiver->SetDirectionWithError(webrtc::RtpTransceiverDirection::kSendRecv);
            RTC_LOG(LS_INFO) << "Initial video transceiver direction set for " << (is_caller() ? "caller" : "callee")
                << ", result:" << (vdirection_result.ok() ? "success" : "failed");
        }

        webrtc::PeerConnectionInterface::RTCOfferAnswerOptions offer_options;

        // Store observer in a member variable to keep it alive
        create_session_observer_ = rtc::make_ref_counted<LambdaCreateSessionDescriptionObserver>(
            [this](std::unique_ptr<webrtc::SessionDescriptionInterface> desc) {
                std::string sdp;
                desc->ToString(&sdp);
                
                // Store observer in a member variable to keep it alive
                set_local_description_observer_ = rtc::make_ref_counted<LambdaSetLocalDescriptionObserver>(
                    [this, sdp](webrtc::RTCError error) {
                        if (!error.ok()) {
                            RTC_LOG(LS_ERROR) << "Failed to set local description: " 
                                              << error.message();
                            signaling_thread()->PostTask([this]() {
                                SendMessage(std::string("BYE"));
                            });
                            return;
                        }
                        RTC_LOG(LS_INFO) << "Local description set successfully";
                        SendMessage(std::string("OFFER:") + sdp);
                    });

                peer_connection_->SetLocalDescription(std::move(desc), set_local_description_observer_);
            });

        peer_connection_->CreateOffer(create_session_observer_.get(), offer_options);
 
     } else {
        RTC_LOG(LS_INFO) << "Waiting for offer...";
        SendMessage("WAITING");
    }
 
  });

}

void DirectPeer::HandleMessage(rtc::AsyncPacketSocket* socket,
                             const std::string& message,
                             const rtc::SocketAddress& remote_addr) {

   if (message.find("INIT") == 0) {
        if (!is_caller()) {
          Start();
        } else {
          RTC_LOG(LS_ERROR) << "Peer is not a callee, cannot init";
        }

   } else if (message == "WAITING") {
        if (is_caller()) {
          Start();
        } else {
          RTC_LOG(LS_ERROR) << "Peer is not a caller, cannot wait";
        }
   } else if (!is_caller() && message.find("OFFER:") == 0) {
      std::string sdp = message.substr(6);  // Use exact length of "OFFER:"
      if(!sdp.empty()) {
        SetRemoteDescription(sdp);
      } else {
        RTC_LOG(LS_ERROR) << "Invalid SDP offer received";
      }
   } else if (is_caller() && message.find("ANSWER:") == 0) {
      std::string sdp = message.substr(7);

      // Got an ANSWER from the callee
      if(sdp.size())
        SetRemoteDescription(sdp);
      else
        RTC_LOG(LS_ERROR) << "Invalid SDP answer received";

   } else if (message.find("ICE:") == 0) {
     std::string candidate = message.substr(4);
      if (!candidate.empty()) {
          RTC_LOG(LS_INFO) << "Received ICE candidate: " << candidate;
          AddIceCandidate(candidate);
      } else {
          RTC_LOG(LS_ERROR) << "Invalid ICE candidate received";
      }      
   } else {
       DirectApplication::HandleMessage(socket, message, remote_addr);
   }
}

bool DirectPeer::SendMessage(const std::string& message) {
    
    return DirectApplication::SendMessage(message);
}

void DirectPeer::OnIceCandidate(const webrtc::IceCandidateInterface* candidate) {
    std::string sdp;
    if (!candidate->ToString(&sdp)) {
        RTC_LOG(LS_ERROR) << "Failed to serialize candidate";
        return;
    }

    RTC_LOG(LS_INFO) << "New ICE candidate: " << sdp 
                     << " mid: " << candidate->sdp_mid()
                     << " mlineindex: " << candidate->sdp_mline_index();
    
    // Only send ICE candidates after local description is set
    if (!peer_connection_->local_description()) {
        RTC_LOG(LS_INFO) << "Queuing ICE candidate until local description is set";
        pending_ice_candidates_.push_back(sdp);
        return;
    }
    
    SendMessage("ICE:" + sdp);
}

void DirectPeer::SetRemoteDescription(const std::string& sdp) {
    if (!peer_connection()) {
        RTC_LOG(LS_ERROR) << "PeerConnection not initialized...";
        return;
    }
  
    signaling_thread()->PostTask([this, sdp]() {
        RTC_LOG(LS_INFO) << "Processing remote description as " 
                        << (is_caller() ? "ANSWER" : "OFFER");
        
        webrtc::SdpParseError error;
        webrtc::SdpType sdp_type = is_caller() ? webrtc::SdpType::kAnswer 
                                             : webrtc::SdpType::kOffer;
        
        std::unique_ptr<webrtc::SessionDescriptionInterface> session_description =
            webrtc::CreateSessionDescription(sdp_type, sdp, &error);
            
        if (!session_description) {
            RTC_LOG(LS_ERROR) << "Failed to parse remote SDP: " << error.description;
            return;
        }

        auto observer = rtc::make_ref_counted<LambdaSetRemoteDescriptionObserver>(
            [this](webrtc::RTCError error) {
                if (!error.ok()) {
                    RTC_LOG(LS_ERROR) << "Failed to set remote description: " 
                                    << error.message();
                    return;
                }
                RTC_LOG(LS_INFO) << "Remote description set successfully";
                auto transceivers = peer_connection()->GetTransceivers();
                RTC_DCHECK(transceivers.size() > 0);
                auto transceiver = transceivers[0];

                // Force send/recv mode
                auto result = transceiver->SetDirectionWithError(webrtc::RtpTransceiverDirection::kSendRecv);
                if (!result.ok()) {
                    RTC_LOG(LS_ERROR) << "Failed to set transceiver direction: " << result.message();
                }
                
                webrtc::RtpTransceiverDirection direction = transceiver->direction();
                RTC_LOG(LS_INFO) << "Transceiver direction is " << 
                    (direction == webrtc::RtpTransceiverDirection::kSendRecv ? "send/rcv" : 
                     direction == webrtc::RtpTransceiverDirection::kRecvOnly ? "recv-only" : "other");               

                if (!is_caller() && 
                    peer_connection()->signaling_state() == 
                        webrtc::PeerConnectionInterface::kHaveRemoteOffer) {
                    RTC_LOG(LS_INFO) << "Creating answer as callee...";

                    // Add local audio track for callee
                    cricket::AudioOptions audio_options;
                    auto audio_source = peer_connection_factory_->CreateAudioSource(audio_options);
                    RTC_DCHECK(audio_source.get());
                    audio_track_ = peer_connection_factory_->CreateAudioTrack(std::string("audio_track"), audio_source.get());
                    RTC_DCHECK(audio_track_.get());
                    webrtc::RtpTransceiverInit ainit;
                    ainit.direction = webrtc::RtpTransceiverDirection::kSendRecv;
                    auto at_result = peer_connection()->AddTransceiver(audio_track_, ainit);
                    RTC_DCHECK(at_result.ok());
                    auto atransceiver = at_result.value();
                    auto adirection_result = atransceiver->SetDirectionWithError(webrtc::RtpTransceiverDirection::kSendRecv);
                    RTC_LOG(LS_INFO) << "Initial audio transceiver direction set for callee, result: " \
                                     << (adirection_result.ok() ? "success" : "failed");

                    // Add local video track for callee
                    if (opts_.video) {
                        if (!video_source_) {
                            // Create a static periodic video source (local fake source)
                            auto static_source = rtc::make_ref_counted<webrtc::StaticPeriodicVideoTrackSource>(false);
                            // Load custom YUV data if available
                            if (opts_.llama && !opts_.llama_llava_yuv.empty()) {
                                RTC_LOG(LS_INFO) << "Loading YUV data from " << opts_.llama_llava_yuv;
                                YUVData &yuv = webrtc::SpeechAudioDeviceFactory::GetYuvData();
                                if(yuv.width > 0 && yuv.height > 0) {
                                    static_source->static_periodic_source().LoadYuvData(yuv);
                                } else {
                                    RTC_LOG(LS_ERROR) << "Invalid YUV data";
                                }
                            }
                            video_source_ = static_source;
                            RTC_LOG(LS_INFO) << "Created StaticPeriodicVideoTrackSource";
                        }

                        video_track_ = peer_connection_factory_->CreateVideoTrack(video_source_, std::string("video_track"));
                        RTC_DCHECK(video_track_.get());
                        webrtc::RtpTransceiverInit vinit;
                        vinit.direction = webrtc::RtpTransceiverDirection::kSendRecv;
                        auto vt_result = peer_connection()->AddTransceiver(video_track_, vinit);
                        RTC_DCHECK(vt_result.ok());
                        auto vtransceiver = vt_result.value();
                        auto vdirection_result = vtransceiver->SetDirectionWithError(webrtc::RtpTransceiverDirection::kSendRecv);
                        RTC_LOG(LS_INFO) << "Initial video transceiver direction set for callee, result: " \
                                         << (vdirection_result.ok() ? "success" : "failed");
                    }

                    create_session_observer_ = rtc::make_ref_counted<LambdaCreateSessionDescriptionObserver>(
                        [this](std::unique_ptr<webrtc::SessionDescriptionInterface> desc) {
                            std::string sdp;
                            desc->ToString(&sdp);
                            
                            set_local_description_observer_ = rtc::make_ref_counted<LambdaSetLocalDescriptionObserver>(
                                [this, sdp](webrtc::RTCError error) {
                                    if (!error.ok()) {
                                        RTC_LOG(LS_ERROR) << "Failed to set local description: " 
                                                        << error.message();
                                        signaling_thread()->PostTask([this]() {
                                            SendMessage(std::string("BYE"));
                                        });
                                        return;
                                    }
                                    RTC_LOG(LS_INFO) << "Local description set successfully";
                                    SendMessage(std::string("ANSWER:") + sdp);
                            });

                            peer_connection_->SetLocalDescription(std::move(desc), set_local_description_observer_);
                        });
                        
                    peer_connection_->CreateAnswer(
                        create_session_observer_.get(), webrtc::PeerConnectionInterface::RTCOfferAnswerOptions{});
                }
            });

        peer_connection_->SetRemoteDescription(std::move(session_description), observer);
    });
}

void DirectPeer::AddIceCandidate(const std::string& candidate_sdp) {
    signaling_thread()->PostTask([this, candidate_sdp]() {
        // Simply queue if descriptions aren't ready
        if (!peer_connection_->remote_description() || !peer_connection_->local_description()) {
            RTC_LOG(LS_INFO) << "Queuing ICE candidate - descriptions not ready";
            pending_ice_candidates_.push_back(candidate_sdp);
            return;
        }

        webrtc::SdpParseError error;
        std::unique_ptr<webrtc::IceCandidateInterface> candidate(
            webrtc::CreateIceCandidate("0", 0, candidate_sdp, &error));
        if (!candidate) {
            RTC_LOG(LS_ERROR) << "Failed to parse ICE candidate: " << error.description;
            return;
        }

        RTC_LOG(LS_INFO) << "Adding ICE candidate";
        peer_connection_->AddIceCandidate(candidate.get());
    });
}

// PeerConnectionObserver implementation
void DirectPeer::OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState new_state) {
    const char* state_names[] = {
        "Stable", "HaveLocalOffer", "HaveLocalPrAnswer", "HaveRemoteOffer", "HaveRemotePrAnswer", "Closed"
    };
    const char* state_name = (new_state < 6) ? state_names[new_state] : "Unknown";
    
    RTC_LOG(LS_INFO) << "PeerConnection SignalingState changed to: " 
                     << static_cast<int>(new_state) << " (" << state_name << ")";
    
    // If the signaling state goes to Closed, also signal connection closed
    if (new_state == webrtc::PeerConnectionInterface::kClosed) {
        RTC_LOG(LS_INFO) << "PeerConnection signaling state is Closed. Signaling connection_closed_event.";
        connection_closed_event_.Set();
    }
}

void DirectPeer::OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState new_state) {
    const char* state_names[] = {
        "New", "Checking", "Connected", "Completed", "Failed", "Disconnected", "Closed", "Max"
    };
    const char* state_name = (new_state < 7) ? state_names[new_state] : "Unknown";
    
    RTC_LOG(LS_INFO) << "PeerConnection IceConnectionState changed to: " 
                     << static_cast<int>(new_state) << " (" << state_name << ")";
    
    // Handle successful connection
    if (new_state == webrtc::PeerConnectionInterface::kIceConnectionConnected ||
        new_state == webrtc::PeerConnectionInterface::kIceConnectionCompleted) {
        RTC_LOG(LS_INFO) << "Connection established successfully!";
    }
    
    // Handle connection failure, disconnection, or closure
    if (new_state == webrtc::PeerConnectionInterface::kIceConnectionFailed ||
        new_state == webrtc::PeerConnectionInterface::kIceConnectionDisconnected ||
        new_state == webrtc::PeerConnectionInterface::kIceConnectionClosed) {
        RTC_LOG(LS_INFO) << "Connection failed/disconnected/closed. Signaling connection_closed_event.";
        connection_closed_event_.Set(); // Signal the event to restart the callee
    }
}

// Method for external logic to wait for the closed signal
bool DirectPeer::WaitUntilConnectionClosed(int give_up_after_ms) {
    RTC_LOG(LS_INFO) << "Waiting for connection closed event...";
    
    // Check should_quit_ immediately and periodically during wait
    if (should_quit_) {
        RTC_LOG(LS_INFO) << "Quit signal detected, returning immediately";
        return true; // Return true to break the calling loop
    }
    
    // Wait in smaller chunks to check quit flag periodically
    int chunk_size = std::min(give_up_after_ms, 100); // 100ms chunks
    int remaining = give_up_after_ms;
    
    while (remaining > 0 && !should_quit_) {
        int current_wait = std::min(remaining, chunk_size);
        if (connection_closed_event_.Wait(webrtc::TimeDelta::Millis(current_wait))) {
            RTC_LOG(LS_INFO) << "Connection closed event received";
            return true; // Event was signaled
        }
        remaining -= current_wait;
        
        // Check quit flag again
        if (should_quit_) {
            RTC_LOG(LS_INFO) << "Quit signal detected during wait, exiting";
            return true; // Return true to break the calling loop
        }
    }
    
    // Timeout reached
    RTC_LOG(LS_INFO) << "Wait timeout reached or quit requested";
    return false;
}

// Method to reset the event before a new connection attempt
void DirectPeer::ResetConnectionClosedEvent() {
    connection_closed_event_.Reset();
}
