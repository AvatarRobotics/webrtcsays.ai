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

#if defined(WEBRTC_IOS) && defined(__OBJC__)
#import "direct.h"
#import "sdk/objc/base/RTCVideoCapturer.h"
#import "sdk/objc/components/renderer/metal/RTCMTLVideoView.h"
#import "sdk/objc/native/src/objc_video_track_source.h"
#import "sdk/objc/native/src/objc_video_renderer.h"
#include <memory>

// Convenience shim: take an Obj-C RTCVideoCapturer and wrap it into WebRTC's
// ObjCVideoTrackSource, then inject into DirectApplication.
void DirectApplication::SetVideoCapturer(RTCVideoCapturer* capturer) {
  // Create adapter that implements RTCVideoCapturerDelegate
  RTCObjCVideoSourceAdapter* adapter = [[RTCObjCVideoSourceAdapter alloc] init];
  // Point capturer at our adapter
  capturer.delegate = adapter;
  // Wrap adapter in a native VideoTrackSource
  auto native_source = rtc::make_ref_counted<webrtc::ObjCVideoTrackSource>(adapter);
  // Hand it to the C++ engine
  SetVideoSource(native_source);
}

void DirectApplication::SetVideoRenderer(RTCMTLVideoView* renderer) {
  // Wrap the provided Obj-C view into a native VideoSink
  std::unique_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> native_sink =
      std::make_unique<webrtc::ObjCVideoRenderer>(renderer);
  // Hand it to the C++ engine
  SetVideoSink(std::move(native_sink));
}
#endif  // defined(WEBRTC_IOS) && defined(__OBJC__)