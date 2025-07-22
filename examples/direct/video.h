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

#ifndef DIRECT_STATIC_H_
#define DIRECT_STATIC_H_

#include <memory>
#include <cstring>

#include "api/video/video_source_interface.h"
#include "media/base/fake_frame_source.h"
#include "media/base/video_broadcaster.h"
#include "rtc_base/synchronization/mutex.h"
#include "rtc_base/task_queue_for_test.h"
#include "rtc_base/task_utils/repeating_task.h"
#include "pc/video_track_source.h"

#include "api/video/i420_buffer.h"
#include "modules/third_party/whillats/src/whillats.h"
#include "modules/audio_device/speech/speech_audio_device_factory.h"
#include "rtc_base/time_utils.h"

namespace webrtc {
class StaticPeriodicVideoSource final
    : public rtc::VideoSourceInterface<VideoFrame> {
 public:
  static constexpr int kDefaultFrameIntervalMs = 1000;
  static constexpr int kDefaultWidth = 640;
  static constexpr int kDefaultHeight = 480;

  struct Config {
    int width = kDefaultWidth;
    int height = kDefaultHeight;
    int frame_interval_ms = kDefaultFrameIntervalMs;
    VideoRotation rotation = kVideoRotation_0;
    int64_t timestamp_offset_ms = 0;
  };

  StaticPeriodicVideoSource() : StaticPeriodicVideoSource(Config()) {}
  explicit StaticPeriodicVideoSource(Config config)
      : frame_source_(
            config.width,
            config.height,
            config.frame_interval_ms * rtc::kNumMicrosecsPerMillisec,
            config.timestamp_offset_ms * rtc::kNumMicrosecsPerMillisec),
        task_queue_(std::make_unique<TaskQueueForTest>(
            "FakePeriodicVideoTrackSource")),
        rotation_(config.rotation),
        current_width_(config.width),
        current_height_(config.height) {
    frame_source_.SetRotation(config.rotation);

    TimeDelta frame_interval = TimeDelta::Millis(config.frame_interval_ms);
    repeating_task_handle_ =
        RepeatingTaskHandle::Start(task_queue_->Get(), [this, frame_interval] {
          // Only send frames if a YUV buffer is loaded - don't send black/fake frames
          {
            MutexLock lock(&mutex_);
            rtc::scoped_refptr<VideoFrameBuffer> buffer_to_send;
            if (use_yuv_ && yuv_buffer_) {
              buffer_to_send = yuv_buffer_;
            } else {
              buffer_to_send = I420Buffer::Create(current_width_, current_height_);
            }

            int64_t timestamp = rtc::TimeMicros();
            VideoFrame frame(buffer_to_send, rotation_, timestamp);
            broadcaster_.OnFrame(frame);
            return frame_interval;
          }
          // No YUV buffer loaded - skip sending frame to avoid black frames
          // This ensures only real video content is transmitted
          return frame_interval;
        });
  }

  rtc::VideoSinkWants wants() const {
    MutexLock lock(&mutex_);
    return wants_;
  }

  void RemoveSink(rtc::VideoSinkInterface<VideoFrame>* sink) override {
    RTC_DCHECK(thread_checker_.IsCurrent());
    broadcaster_.RemoveSink(sink);
  }

  void AddOrUpdateSink(rtc::VideoSinkInterface<VideoFrame>* sink,
                       const rtc::VideoSinkWants& wants) override {
    RTC_DCHECK(thread_checker_.IsCurrent());
    {
      MutexLock lock(&mutex_);
      wants_ = wants;
    }
    broadcaster_.AddOrUpdateSink(sink, wants);
  }

  void Stop() {
    RTC_DCHECK(task_queue_);
    task_queue_->SendTask([&]() { repeating_task_handle_.Stop(); });
    task_queue_.reset();
  }

  // Allow loading custom YUV data to be broadcast periodically.
  void LoadYuvData(const YUVData& data) {
    // Create an I420 buffer and copy YUV planes
    auto buffer = I420Buffer::Create(data.width, data.height);
    // Copy Y plane
    for (int i = 0; i < data.height; ++i) {
      memcpy(buffer->MutableDataY() + i * buffer->StrideY(),
             data.y.get() + i * data.width,
             data.width);
    }
    // Copy U and V planes (half resolution)
    int half_width = data.width / 2;
    int half_height = data.height / 2;
    for (int i = 0; i < half_height; ++i) {
      memcpy(buffer->MutableDataU() + i * buffer->StrideU(),
             data.u.get() + i * half_width,
             half_width);
      memcpy(buffer->MutableDataV() + i * buffer->StrideV(),
             data.v.get() + i * half_width,
             half_width);
    }
    // Store under lock and update dimensions
    {
      MutexLock lock(&mutex_);
      yuv_buffer_ = buffer;
      use_yuv_ = true;
      current_width_ = data.width;
      current_height_ = data.height;
    }
  }

  bool is_running() const {
    return repeating_task_handle_.Running();
  }

 private:
  SequenceChecker thread_checker_{SequenceChecker::kDetached};

  rtc::VideoBroadcaster broadcaster_;
  cricket::FakeFrameSource frame_source_;
  mutable Mutex mutex_;
  rtc::VideoSinkWants wants_ RTC_GUARDED_BY(&mutex_);

  std::unique_ptr<TaskQueueForTest> task_queue_;
  RepeatingTaskHandle repeating_task_handle_;

  // Custom YUV buffer and flag
  rtc::scoped_refptr<I420BufferInterface> yuv_buffer_ RTC_GUARDED_BY(&mutex_);
  bool use_yuv_ RTC_GUARDED_BY(&mutex_) = false;
  VideoRotation rotation_;
  // Dynamic dimensions for frames
  int current_width_ RTC_GUARDED_BY(&mutex_);
  int current_height_ RTC_GUARDED_BY(&mutex_);
};

class StaticPeriodicVideoTrackSource : public VideoTrackSource {
 public:
  explicit StaticPeriodicVideoTrackSource(bool remote)
      : StaticPeriodicVideoTrackSource(StaticPeriodicVideoSource::Config(),
                                     remote) {}

  StaticPeriodicVideoTrackSource(StaticPeriodicVideoSource::Config config,
                               bool remote)
      : VideoTrackSource(remote), source_(config) {}

  ~StaticPeriodicVideoTrackSource() = default;

  StaticPeriodicVideoSource& static_periodic_source() { return source_; }
  const StaticPeriodicVideoSource& static_periodic_source() const {
    return source_;
  }

  bool is_running() const { return state() == webrtc::MediaSourceInterface::kLive; }

 protected:
  rtc::VideoSourceInterface<VideoFrame>* source() override { return &source_; }

 private:
  StaticPeriodicVideoSource source_;
};

class EchoVideoTrackSource : public webrtc::VideoTrackSource,
                            public rtc::VideoSinkInterface<webrtc::VideoFrame> {
 public:
  EchoVideoTrackSource()
      : webrtc::VideoTrackSource(/*remote=*/false) {
     SetState(webrtc::MediaSourceInterface::kLive);
   }

  // rtc::VideoSinkInterface implementation – called with frames from the
  // *remote* video track we attach to.
  void OnFrame(const webrtc::VideoFrame& frame) override {
    broadcaster_.OnFrame(frame);
  }
  void OnDiscardedFrame() override {}

  // Expose wants aggregation from broadcaster
  rtc::VideoSinkWants wants() const { return broadcaster_.wants(); }

 protected:
  // webrtc::VideoTrackSource implementation – this is what the WebRTC encoder
  // pulls frames from.
  rtc::VideoSourceInterface<webrtc::VideoFrame>* source() override {
    return &broadcaster_;
  }

 private:
  rtc::VideoBroadcaster broadcaster_;
};

// Simple video sink that logs frame information to the console
class LlamaVideoRenderer : public rtc::VideoSinkInterface<webrtc::VideoFrame> {
  // Function to check if a WebRTC VideoFrame (I420 format) is black
  bool isVideoFrameBlack(const webrtc::VideoFrame& frame, int threshold = 16) {
      // Get the I420 buffer from the VideoFrame
      rtc::scoped_refptr<webrtc::I420BufferInterface> buffer = frame.video_frame_buffer()->ToI420();
      if (!buffer) {
          return false; // Invalid buffer
      }

      // Get Y plane data, width, height, and stride
      const uint8_t* yPlane = buffer->DataY();
      int width = buffer->width();
      int height = buffer->height();
      int stride = buffer->StrideY();

      // Iterate through the Y plane
      for (int y = 0; y < height; ++y) {
          for (int x = 0; x < width; ++x) {
              // Access Y value at position (x, y)
              uint8_t yValue = yPlane[y * stride + x];
              // If any Y value is above the threshold, the frame is not black
              if (yValue > threshold) {
                  return false;
              }
          }
      }

      // All Y values are below or equal to the threshold
      return true;
  }

 public:
  void set_is_llama(bool is_llama) { is_llama_ = is_llama; }
  bool is_llama() { return is_llama_; }
  
  void OnFrame(const webrtc::VideoFrame& frame) {

    if (!isVideoFrameBlack(frame)) {
      rtc::scoped_refptr<webrtc::VideoFrameBuffer> buffer(
          frame.video_frame_buffer());
      RTC_LOG(LS_VERBOSE) << "Received video frame (" << buffer->type() << ") "
                        << frame.width() << "x" << frame.height()
                        << " timestamp=" << frame.timestamp_us();

      // Convert the frame to I420 format and populate YUVData
      rtc::scoped_refptr<webrtc::I420BufferInterface> i420_buffer = buffer->ToI420();
      if (i420_buffer) {
        size_t y_size = i420_buffer->StrideY() * i420_buffer->height();
        size_t uv_size = i420_buffer->StrideU() * i420_buffer->ChromaHeight();

        YUVData yuv_data;
        yuv_data.width = i420_buffer->width();
        yuv_data.height = i420_buffer->height();
        yuv_data.y_size = y_size;
        yuv_data.uv_size = uv_size;
        yuv_data.y = std::make_unique<uint8_t[]>(y_size);
        std::memcpy(yuv_data.y.get(), i420_buffer->DataY(), y_size);
        yuv_data.u = std::make_unique<uint8_t[]>(uv_size);
        std::memcpy(yuv_data.u.get(), i420_buffer->DataU(), uv_size);
        yuv_data.v = std::make_unique<uint8_t[]>(uv_size);
        std::memcpy(yuv_data.v.get(), i420_buffer->DataV(), uv_size);

        if (is_llama_ && SpeechAudioDeviceFactory::llama()) {
          // Send video frame to be queued for later use with prompts
          SpeechAudioDeviceFactory::llama()->receiveVideoFrame(yuv_data);
          }
      }

      received_frame_ = true;
    } else {
      RTC_LOG(LS_INFO) << "Received black frame!";
    }
  }
 private:
  bool is_llama_ = false;
  bool received_frame_ = false;
};


}  // namespace webrtc

#endif  // DIRECT_STATIC_H_