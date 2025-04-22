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

#include "direct.h"

DirectPeer::DirectPeer(
    Options opts) 
  : DirectApplication(), 
    opts_(opts)
{
}

DirectPeer::~DirectPeer() {
}

void DirectPeer::Shutdown() {
    RTC_LOG(LS_INFO) << "Shutting down peer connection";
    if (peer_connection_) {
        peer_connection_->Close();
        peer_connection_ = nullptr;
    }
    // Do not reset peer_connection_factory_ to allow reconnection
    RTC_LOG(LS_INFO) << "Peer connection closed, but factory preserved for potential reconnection";
}

void DirectPeer::Start() {

  signaling_thread()->PostTask([this]() {

    if(peer_connection_ == nullptr) {
        if (!CreatePeerConnection(opts_)) {
            RTC_LOG(LS_ERROR) << "Failed to create peer connection";
            return;
        }
    }

    if (is_caller()) {
        cricket::AudioOptions audio_options;

        auto audio_source = peer_connection_factory_->CreateAudioSource(audio_options);
        RTC_DCHECK(audio_source.get());
        audio_track_ = peer_connection_factory_->CreateAudioTrack("audio_track", audio_source.get());
        RTC_DCHECK(audio_track_.get());

        webrtc::RtpTransceiverInit init;
        init.direction = webrtc::RtpTransceiverDirection::kSendRecv;
        auto at_result = peer_connection_->AddTransceiver(audio_track_, init);
        RTC_DCHECK(at_result.ok());
        auto transceiver = at_result.value();

        // Force the direction immediately after creation
        auto direction_result = transceiver->SetDirectionWithError(webrtc::RtpTransceiverDirection::kSendRecv);
        RTC_LOG(LS_INFO) << "Initial transceiver direction set: " << 
            (direction_result.ok() ? "success" : "failed");
    
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
                                SendMessage("BYE");
                            });
                            return;
                        }
                        RTC_LOG(LS_INFO) << "Local description set successfully";
                        SendMessage("OFFER:" + sdp);
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
   RTC_LOG(LS_INFO) << "[Peer::HandleMessage] Received message: '" << message << "'";

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

// PeerConnectionObserver implementation
void DirectPeer::OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState new_state) {
    // Implementation will go here
}

void DirectPeer::OnAddTrack(rtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver,
                           const std::vector<rtc::scoped_refptr<webrtc::MediaStreamInterface>>& streams) {
    // Implementation will go here
}

void DirectPeer::OnRemoveTrack(rtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver) {
    // Implementation will go here
}

void DirectPeer::OnDataChannel(rtc::scoped_refptr<webrtc::DataChannelInterface> channel) {
    // Implementation will go here
}

void DirectPeer::OnRenegotiationNeeded() {
    // Implementation will go here
}

void DirectPeer::OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState new_state) {
    // Implementation will go here
}

void DirectPeer::OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState new_state) {
  switch (new_state) {
    case webrtc::PeerConnectionInterface::kIceGatheringNew:
      RTC_LOG(LS_INFO) << "ICE gathering state: New - Starting to gather candidates";
      break;
    case webrtc::PeerConnectionInterface::kIceGatheringGathering:
      RTC_LOG(LS_INFO) << "ICE gathering state: Gathering - Collecting candidates";
      break;
    case webrtc::PeerConnectionInterface::kIceGatheringComplete:
      RTC_LOG(LS_INFO) << "ICE gathering state: Complete - All candidates collected";
      break;
  }
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

void DirectPeer::OnIceConnectionReceivingChange(bool receiving) {
    // Implementation will go here
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
                                            SendMessage("BYE");
                                        });
                                        return;
                                    }
                                    RTC_LOG(LS_INFO) << "Local description set successfully";
                                    SendMessage("ANSWER:" + sdp);
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

