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
#include <thread>
#include <algorithm>
#include <cctype>

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
#include "option.h"
#include "direct.h"
#include "pc/test/fake_video_track_source.h"
#include "test/vcm_capturer.h"
#include "test/test_video_capturer.cc"
#include "test/vcm_capturer.cc"

// Helper sink that forwards captured frames to a FakeVideoTrackSource so that
// the source can be used as a regular WebRTC VideoTrackSourceInterface.
class CameraFrameSink : public rtc::VideoSinkInterface<webrtc::VideoFrame> {
 public:
  explicit CameraFrameSink(const rtc::scoped_refptr<webrtc::FakeVideoTrackSource>& src)
      : src_(src) {}

  void OnFrame(const webrtc::VideoFrame& frame) override {
    if (src_) {
      src_->InjectFrame(frame);
    }
  }

 private:
  rtc::scoped_refptr<webrtc::FakeVideoTrackSource> src_;
};

// helper to trim whitespace
auto trim = [](std::string s){
    size_t start = s.find_first_not_of(" \t\n\r");
    size_t end   = s.find_last_not_of(" \t\n\r");
    if (start==std::string::npos) return std::string();
    return s.substr(start, end-start+1);
};

// Parse camera option of the form "<index>[,<width>x<height>@<fps>]" or
// "<name>[,<width>x<height>@<fps>]". Missing parts fall back to defaults.
static bool ParseCameraSpec(const std::string& spec,
                            std::string* out_device,
                            size_t* out_index,
                            int* out_width,
                            int* out_height,
                            int* out_fps) {
  if (spec.empty()) return false;

  std::string cleaned = spec;
  if (cleaned.size()>=2 && ((cleaned.front()=='"'&&cleaned.back()=='"')||(cleaned.front()=='\''&&cleaned.back()=='\''))) {
      cleaned = cleaned.substr(1, cleaned.size()-2);
  }
  cleaned = trim(cleaned);

  // Defaults.
  *out_device = "";
  *out_index  = 0;
  *out_width  = 640;
  *out_height = 480;
  *out_fps    = 30;

  // Split on first comma – everything after is format.
  size_t comma = cleaned.find(',');
  std::string device_part = cleaned.substr(0, comma);
  std::string fmt_part    = (comma != std::string::npos) ? cleaned.substr(comma + 1) : "";

  // Try numeric device index first.
  bool numeric = !device_part.empty() && std::all_of(device_part.begin(), device_part.end(), ::isdigit);
  if (numeric) {
    *out_index = static_cast<size_t>(std::stoul(device_part));
  } else {
    *out_device = device_part;
  }

  if (!fmt_part.empty()) {
    // Expected "<width>x<height>@<fps>" – each optional.
    int w = *out_width, h = *out_height, fps = *out_fps;
    size_t x_pos = fmt_part.find('x');
    size_t at_pos = fmt_part.find('@');
    if (x_pos != std::string::npos) {
      w = std::stoi(fmt_part.substr(0, x_pos));
      if (at_pos != std::string::npos) {
        h = std::stoi(fmt_part.substr(x_pos + 1, at_pos - x_pos - 1));
        fps = std::stoi(fmt_part.substr(at_pos + 1));
      } else {
        h = std::stoi(fmt_part.substr(x_pos + 1));
      }
    }
    *out_width  = w;
    *out_height = h;
    *out_fps    = fps;
  }
  return true;
}

class DirectPeer; // forward declaration

rtc::scoped_refptr<webrtc::VideoTrackSourceInterface>
CreateCameraVideoSource(DirectPeer* owner, const Options& opts) {
  using namespace webrtc;

  std::string device_name;
  size_t      device_index = 0;
  int         width = 640, height = 480, fps = 30;

  if (!ParseCameraSpec(opts.camera, &device_name, &device_index, &width, &height, &fps)) {
    return nullptr;
  }

  // Enumerate devices if name was supplied or to validate index.
  std::unique_ptr<VideoCaptureModule::DeviceInfo> dev_info(VideoCaptureFactory::CreateDeviceInfo());
  if (!dev_info) {
    RTC_LOG(LS_ERROR) << "Camera DeviceInfo unavailable";
    return nullptr;
  }

  uint32_t num_devs = dev_info->NumberOfDevices();
  if (num_devs == 0) {
    RTC_LOG(LS_ERROR) << "No video capture devices found";
    return nullptr;
  }

  // If device name requested, translate to index.
  if (!device_name.empty()) {
    bool found = false;
    char name[256];
    char unique_name[256];
    for (uint32_t i = 0; i < num_devs; ++i) {
      if (dev_info->GetDeviceName(i, name, sizeof(name), unique_name, sizeof(unique_name)) != 0) {
        continue;
      }
      if (device_name == name || device_name == unique_name) {
        device_index = i;
        found = true;
        break;
      }
    }
    if (!found) {
      RTC_LOG(LS_ERROR) << "Requested camera '" << device_name << "' not found";
      return nullptr;
    }
  } else if (device_index >= num_devs) {
    RTC_LOG(LS_ERROR) << "Camera index " << device_index << " out of range (" << num_devs << ")";
    return nullptr;
  }

  // Build the FakeVideoTrackSource on the owner's signaling thread so its
  // internal thread checker is satisfied.
  rtc::scoped_refptr<FakeVideoTrackSource> track_source;
  if (owner && owner->signaling_thread()) {
      owner->signaling_thread()->BlockingCall([&track_source]() {
          track_source = FakeVideoTrackSource::Create(false);
          track_source->SetState(webrtc::MediaSourceInterface::kLive);
      });
  } else {
      track_source = FakeVideoTrackSource::Create(false);
      track_source->SetState(webrtc::MediaSourceInterface::kLive);
  }

  // Create capturer.
  std::unique_ptr<webrtc::test::VcmCapturer> capturer(
      webrtc::test::VcmCapturer::Create(static_cast<size_t>(width), static_cast<size_t>(height), static_cast<size_t>(fps), device_index));

  if (!capturer) {
    RTC_LOG(LS_ERROR) << "Failed to create VcmCapturer for camera index " << device_index;
    return nullptr;
  }

  // Create sink to forward frames.
  std::unique_ptr<CameraFrameSink> sink = std::make_unique<CameraFrameSink>(track_source);
  capturer->AddOrUpdateSink(sink.get(), rtc::VideoSinkWants());

  if (owner) {
    owner->capturers().push_back(std::move(capturer));
    owner->sinks().push_back(std::move(sink));
  }

  RTC_LOG(LS_INFO) << "Camera video source created: " << width << "x" << height << "@" << fps << "fps";

  return track_source;
}

#include "video.h"
#include "status.h"

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
    RTC_LOG(LS_INFO) << "Current peer connection state before shutdown: " << (peer_connection_ ? "Active" : "Inactive");

    // Clear pointer now so "busy" checks immediately reflect idle state.
    auto pc_to_close = std::move(peer_connection_);

    signaling_thread()->PostTask([this, pc = std::move(pc_to_close)]() mutable {
        //RTC_DCHECK_RUN_ON(signaling_thread());
        // Release tracks first so their destructors can safely invoke into WebRTC threads
        audio_track_ = nullptr;
        video_track_ = nullptr;
        video_source_ = nullptr;
        RTC_LOG(LS_INFO) << "Media tracks and sources released on signaling thread.";
        // Now close the PeerConnection, after tracks are released
        if (pc) {
            pc->Close();
            RTC_LOG(LS_INFO) << "Peer connection closed on signaling thread.";
            pc = nullptr;
        } else {
            RTC_LOG(LS_INFO) << "No active peer connection to close.";
        }
        // Defer factory destruction to main thread (non-blocking)
    });

    // Factory reset can happen asynchronously; no blocking ops needed.
    main_thread()->PostTask([this]() { peer_connection_factory_ = nullptr; });

    RTC_LOG(LS_INFO) << "Peer connection and factory closed and released on signaling thread. Peer connection factory preserved for reconnection.";
}

void DirectPeer::Start() {

  // Reset the closed event before starting a new connection attempt
  ResetConnectionClosedEvent();
  
  // Removed the 30-second timeout watchdog – some networks may legitimately
  // need longer to finish ICE checks or STUN/TURN negotiations.  The session
  // will now stay in the "checking" state until the peer-connection itself
  // reports success or failure.

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
    
        // Create a video track source for the caller.
        if (opts_.video) {
            // Reuse existing live source if present.
            rtc::scoped_refptr<webrtc::VideoTrackSourceInterface> existing_source = video_source_;
            if (existing_source && existing_source->state() == webrtc::MediaSourceInterface::kLive) {
                RTC_LOG(LS_INFO) << "Existing video source is already live – keeping it.";
            } else {
                // Prefer real camera capture when opts_.camera is provided.
                if (!opts_.camera.empty()) {
                    video_source_ = CreateCameraVideoSource(this, opts_);
                }
                // Fallback to synthetic if camera unavailable or unspecified.
                if (!video_source_) {
                    RTC_LOG(LS_INFO) << "Falling back to StaticPeriodicVideoTrackSource";
                    signaling_thread()->BlockingCall([this]() {
                        auto* src = new rtc::RefCountedObject<
                            webrtc::StaticPeriodicVideoTrackSource>(false);
                        src->SetState(webrtc::MediaSourceInterface::kLive);
                        video_source_ = src;               // assign while on signaling thread
                    });
                }
                SetVideoSource(video_source_);
            }
            RTC_LOG(LS_INFO) << "Video source configured for caller.";
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
                        // Now that both descriptions may be in place, process any queued ICE candidates.
                        DrainPendingIceCandidates();
                        SendMessage(std::string(Msg::kOfferPrefix) + sdp);
                    });

                peer_connection_->SetLocalDescription(std::move(desc), set_local_description_observer_);
            });

        peer_connection_->CreateOffer(create_session_observer_.get(), offer_options);
 
     } else {
        RTC_LOG(LS_INFO) << "Waiting for offer...";
        SendMessage(Msg::kWaiting);
    }
 
  });

}

void DirectPeer::HandleMessage(rtc::AsyncPacketSocket* socket,
                             const std::string& message,
                             const rtc::SocketAddress& remote_addr) {

   if (message.rfind(Msg::kInvite, 0) == 0) {
        // Default agent capability
        remote_agent_ = "audio";

        // Parse optional JSON payload after "INVITE:"
        constexpr size_t kPrefixLen = sizeof(Msg::kInvite) - 1; // length of "INVITE"
        if (message.size() > kPrefixLen + 1 && message[kPrefixLen] == ':') {
          std::string json = message.substr(kPrefixLen + 1);
          size_t pos = json.find("\"agent\"");
          if (pos != std::string::npos) {
            pos = json.find(':', pos);
            if (pos != std::string::npos) {
              size_t start = json.find('"', pos+1);
              if (start != std::string::npos) {
                size_t end = json.find('"', start+1);
                if (end != std::string::npos) {
                  remote_agent_ = json.substr(start+1, end-start-1);
                  RTC_LOG(LS_INFO) << "Remote agent capability set to '" << remote_agent_ << "'";
                }
              }
            }
          }
        }

        if (!is_caller()) {
          Start();
        } else {
          RTC_LOG(LS_ERROR) << "Peer is not a callee, cannot init";
        }

   } else if (message == Msg::kWaiting) {
        if (is_caller()) {
          Start();
        } else {
          RTC_LOG(LS_ERROR) << "Peer is not a caller, cannot wait";
        }
   } else if (!is_caller() && message.find(Msg::kOfferPrefix) == 0) {
      // --------------------------------------------------------------
      // OFFER (callee side) – may arrive in several TCP fragments.
      // --------------------------------------------------------------
      const size_t prefix_len = sizeof(Msg::kOfferPrefix) - 1; // length of "OFFER:"
      std::string sdp_fragment = message.substr(prefix_len);

      pending_remote_sdp_ += sdp_fragment;

      // Try parsing the accumulated buffer to see if we now have the full SDP.
      webrtc::SdpParseError err;
      std::unique_ptr<webrtc::SessionDescriptionInterface> test_desc =
          webrtc::CreateSessionDescription(webrtc::SdpType::kOffer, pending_remote_sdp_, &err);

      if (!test_desc) {
        RTC_LOG(LS_WARNING) << "Partial SDP OFFER fragment received – waiting for more (" << err.description << ")";
        return; // wait for more fragments
      }

      // Full SDP successfully parsed – process it.
      std::string complete_sdp = pending_remote_sdp_;
      pending_remote_sdp_.clear();
      SetRemoteDescription(complete_sdp);

   } else if (is_caller() && message.find(Msg::kAnswerPrefix) == 0) {
      // --------------------------------------------------------------
      // ANSWER (caller side) – may arrive in several TCP fragments.
      // --------------------------------------------------------------
      const size_t prefix_len = sizeof(Msg::kAnswerPrefix) - 1; // length of "ANSWER:"
      std::string sdp_fragment = message.substr(prefix_len);

      pending_remote_sdp_ += sdp_fragment;

      webrtc::SdpParseError err;
      std::unique_ptr<webrtc::SessionDescriptionInterface> test_desc =
          webrtc::CreateSessionDescription(webrtc::SdpType::kAnswer, pending_remote_sdp_, &err);

      if (!test_desc) {
        RTC_LOG(LS_WARNING) << "Partial SDP ANSWER fragment received – waiting for more (" << err.description << ")";
        return; // Not complete yet
      }

      std::string complete_sdp = pending_remote_sdp_;
      pending_remote_sdp_.clear();
      SetRemoteDescription(complete_sdp);
    } else if (message.find(Msg::kIcePrefix) == 0) {
      std::string payload = message.substr(sizeof(Msg::kIcePrefix) - 1);
      size_t delim = payload.find(':');
      if (delim == std::string::npos) {
          RTC_LOG(LS_ERROR) << "Malformed ICE payload received: " << payload;
      } else {
          int mline_index = atoi(payload.substr(0, delim).c_str());
          std::string candidate = payload.substr(delim + 1);
          if (!candidate.empty()) {
              RTC_LOG(LS_INFO) << "Received ICE candidate (mline=" << mline_index << ") " << candidate;
              AddIceCandidate(candidate, mline_index);
          } else {
              RTC_LOG(LS_ERROR) << "Invalid ICE candidate received (empty string)";
          }
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

    int mline_index = candidate->sdp_mline_index();

    RTC_LOG(LS_INFO) << "New ICE candidate: " << sdp 
                     << " mid: " << candidate->sdp_mid()
                     << " mlineindex: " << mline_index;
    
    // Only send ICE candidates after local description is set
    if (!peer_connection_->local_description()) {
        RTC_LOG(LS_INFO) << "Queuing ICE candidate until local description is set";
        pending_ice_candidates_.push_back({mline_index, sdp});
        return;
    }

    // Send as ICE:<mline_idx>:<candidate>
    SendMessage(std::string(Msg::kIcePrefix) + std::to_string(mline_index) + ":" + sdp);
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
                // Attempt to process any ICE candidates that arrived early.
                DrainPendingIceCandidates();
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

                    // The remote offer already contains an audio m-line.  Re-use that
                    // transceiver instead of creating an extra one which would cause the
                    // answer to have more m-lines than the offer and therefore make
                    // SetLocalDescription fail.

                    cricket::AudioOptions audio_options;
                    auto audio_source = peer_connection_factory_->CreateAudioSource(audio_options);
                    RTC_DCHECK(audio_source.get());

                    audio_track_ = peer_connection_factory_->CreateAudioTrack("audio_track", audio_source.get());
                    RTC_DCHECK(audio_track_);

                    bool track_added = false;

                    // Try to attach our track to an existing AUDIO transceiver that came
                    // from the remote offer.
                    for (const auto& t : peer_connection()->GetTransceivers()) {
                        if (t->media_type() == cricket::MEDIA_TYPE_AUDIO) {
                            // Make sure we will both send and receive.
                            auto dir_res = t->SetDirectionWithError(webrtc::RtpTransceiverDirection::kSendRecv);
                            RTC_LOG(LS_INFO) << "Setting existing audio transceiver direction → "
                                             << (dir_res.ok() ? "success" : dir_res.message());

                            auto sender = t->sender();
                            if (sender && sender->SetTrack(audio_track_.get())) {
                                RTC_LOG(LS_INFO) << "Attached local audio track to existing transceiver.";
                                track_added = true;
                            } else {
                                RTC_LOG(LS_WARNING) << "Failed to attach track to existing transceiver.";
                            }
                            break; // Only need the first AUDIO transceiver
                        }
                    }

                    // If there was no suitable transceiver (unlikely) fall back to AddTrack which
                    // will re-use or create an appropriate transceiver without adding an extra m-line.
                    if (!track_added) {
                        auto sender_or = peer_connection()->AddTrack(audio_track_, {"stream0"});
                        if (sender_or.ok()) {
                            RTC_LOG(LS_INFO) << "Audio track added via AddTrack fallback.";
                            track_added = true;
                        } else {
                            RTC_LOG(LS_ERROR) << "Failed to add audio track: " << sender_or.error().message();
                        }
                    }

                    if (!track_added) {
                        RTC_LOG(LS_ERROR) << "Could not attach audio track – aborting answer creation.";
                        return;
                    }

                    // Create a video track source for the callee if video is enabled.
                    if (opts_.video) {
                        // Prefer camera when specified.
                        if (!opts_.camera.empty()) {
                            video_source_ = CreateCameraVideoSource(this, opts_);
                        }
                        if (!video_source_) {
                            RTC_LOG(LS_INFO) << "Callee falling back to StaticPeriodicVideoTrackSource";
                            signaling_thread()->BlockingCall([this]() {
                                auto* src = new rtc::RefCountedObject<
                                    webrtc::StaticPeriodicVideoTrackSource>(false);
                                src->SetState(webrtc::MediaSourceInterface::kLive);
                                video_source_ = src;               // assign while on signaling thread
                            });
                        }
                        SetVideoSource(video_source_);
                        RTC_LOG(LS_INFO) << "Video source configured for callee.";
                    }

                    create_session_observer_ = rtc::make_ref_counted<LambdaCreateSessionDescriptionObserver>(
                        [this](std::unique_ptr<webrtc::SessionDescriptionInterface> desc) {
                            std::string sdp;
                            desc->ToString(&sdp);
                            
                            // Ensure DTLS role is passive (server) on callee side to avoid role conflict.
                            #if 0
                            const std::string kActive = "a=setup:active";
                            const std::string kPassive = "a=setup:passive";
                            size_t pos = 0;
                            bool patched = false;
                            while ((pos = sdp.find(kActive, pos)) != std::string::npos) {
                                sdp.replace(pos, kActive.size(), kPassive);
                                pos += kPassive.size();
                                patched = true;
                            }
                            if (patched) {
                                RTC_LOG(LS_INFO) << "DirectPeer(callee): patched setup:active → passive in SDP answer.";
                            }
                            #endif
                            webrtc::SdpParseError perr;
                            std::unique_ptr<webrtc::SessionDescriptionInterface> patched_desc =
                                webrtc::CreateSessionDescription(desc->GetType(), sdp, &perr);
                            if (!patched_desc) {
                                RTC_LOG(LS_ERROR) << "Failed to reparse patched SDP: " << perr.description;
                                return;
                            }

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
                                    // Both descriptions should now be set on callee side as well.
                                    DrainPendingIceCandidates();
                                    SendMessage(std::string(Msg::kAnswerPrefix) + sdp);
                            });
                            peer_connection_->SetLocalDescription(std::move(patched_desc), set_local_description_observer_);
                        });
                        
                    peer_connection_->CreateAnswer(
                        create_session_observer_.get(), webrtc::PeerConnectionInterface::RTCOfferAnswerOptions{});
                }
            });

        peer_connection_->SetRemoteDescription(std::move(session_description), observer);
    });
}

void DirectPeer::AddIceCandidate(const std::string& candidate_sdp, int mline_index) {
    signaling_thread()->PostTask([this, candidate_sdp, mline_index]() {
        // Queue if descriptions aren't ready yet
        if (!peer_connection_->remote_description() || !peer_connection_->local_description()) {
            RTC_LOG(LS_INFO) << "Queuing ICE candidate - descriptions not ready";
            pending_ice_candidates_.push_back({mline_index, candidate_sdp});
            return;
        }

        webrtc::SdpParseError error;
        std::unique_ptr<webrtc::IceCandidateInterface> candidate(
            webrtc::CreateIceCandidate(std::to_string(mline_index), mline_index, candidate_sdp, &error));
        if (!candidate) {
            RTC_LOG(LS_ERROR) << "Failed to parse ICE candidate: " << error.description;
            return;
        }

        RTC_LOG(LS_INFO) << "Adding ICE candidate (mline=" << mline_index << ")";
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
    RTC_LOG(LS_VERBOSE) << "Waiting for connection closed event...";
    
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
    RTC_LOG(LS_VERBOSE) << "Wait timeout reached or quit requested";
    return false;
}

// Method to reset the event before a new connection attempt
void DirectPeer::ResetConnectionClosedEvent() {
    connection_closed_event_.Reset();
}

// Process any ICE candidates that were received before both the local and
// remote session descriptions were available.  This ensures that early
// candidates are not lost and ICE can proceed once the SDP handshake
// completes.
void DirectPeer::DrainPendingIceCandidates() {
    if (!peer_connection_) {
        return;
    }

    // Both descriptions must be present before we can add candidates.
    if (!peer_connection_->remote_description() || !peer_connection_->local_description()) {
        return;
    }

    for (const auto& item : pending_ice_candidates_) {
        int mline_index = item.first;
        const std::string& candidate_sdp = item.second;

        webrtc::SdpParseError error;
        std::unique_ptr<webrtc::IceCandidateInterface> candidate(
            webrtc::CreateIceCandidate(std::to_string(mline_index), mline_index, candidate_sdp, &error));
        if (!candidate) {
            RTC_LOG(LS_ERROR) << "Failed to parse queued ICE candidate: " << error.description;
            continue;
        }

        RTC_LOG(LS_INFO) << "Adding previously queued ICE candidate (mline=" << mline_index << ")";
        peer_connection_->AddIceCandidate(candidate.get());
    }

    pending_ice_candidates_.clear();
}

// ----------------------------------------------------------------------------
//  DirectPeer – pending‐address helpers shared by caller/callee
// ----------------------------------------------------------------------------

void DirectPeer::ResetCallStartedFlag() {
    // Clear any queued fallback address so the next signalling event starts
    // with a clean slate.
    pending_ip_.clear();
    pending_port_ = 0;

    // Also clear any partially-received SDP that may have accumulated from a
    // previous (now aborted) connection attempt.  This prevents stale or
    // corrupted SDP from leaking into the next call.
    pending_remote_sdp_.clear();
}

bool DirectPeer::initiateWebRTCCall(const std::string& ip, int port) {
    if (!is_caller()) {
        RTC_LOG(LS_WARNING) << "initiateWebRTCCall called on a non-caller peer – ignored.";
        return false;
    }

    // Down-cast to DirectCaller in order to reuse its Connect() overload.
    // RTTI may be disabled (-fno-rtti), therefore avoid dynamic_cast.
    auto* caller = static_cast<DirectCaller*>(this);
    RTC_DCHECK(caller);  // Should always hold because is_caller() was true.

    RTC_LOG(LS_INFO) << "DirectPeer: initiating WebRTC call to " << ip << ":" << port;

    if (!caller->Connect(ip.c_str(), port)) {
        RTC_LOG(LS_ERROR) << "DirectPeer: Connect to " << ip << ":" << port << " failed";
        return false;
    }

    // Launch event loop on a detached thread so signalling continues.
    std::thread([this]() {
        this->RunOnBackgroundThread();
    }).detach();
    return true;
}
