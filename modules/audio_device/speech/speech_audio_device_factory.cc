/*
 *  (c) 2025, wilddolphin2022 
 *  For WebRTCsays.ai project
 *  https://github.com/wilddolphin2022/webrtcsays.ai
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */


#include <stdio.h>
#include <cstdlib>
#include <mutex>

#include "absl/strings/string_view.h"
#include "rtc_base/logging.h"
#include "rtc_base/string_utils.h"

#include "modules/audio_device/speech/speech_audio_device_factory.h"
#include "modules/audio_device/speech/whisper_audio_device.h"

namespace webrtc {

std::unique_ptr<WhillatsTranscriber> SpeechAudioDeviceFactory::_whisperDevice;
std::unique_ptr<WhillatsLlama> SpeechAudioDeviceFactory::_llamaDevice;
std::unique_ptr<WhillatsTTS> SpeechAudioDeviceFactory::_ttsDevice;

std::string SpeechAudioDeviceFactory::_whisperModelFilename;
std::string SpeechAudioDeviceFactory::_llamaModelFilename;
std::string SpeechAudioDeviceFactory::_llavaMMProjFilename;
std::string SpeechAudioDeviceFactory::_wavFilename;
std::string SpeechAudioDeviceFactory::_yuvFilename;

TaskQueueFactory* SpeechAudioDeviceFactory::_taskQueueFactory;

WhillatsTTS* SpeechAudioDeviceFactory::CreateWhillatsTTS(WhillatsSetAudioCallback &ttsCallback) {
  _ttsDevice = std::make_unique<WhillatsTTS>(ttsCallback);
  return _ttsDevice.get();
}

WhillatsTranscriber* SpeechAudioDeviceFactory::CreateWhillatsTranscriber
  (WhillatsSetResponseCallback &whisperCallback, WhillatsSetLanguageCallback &languageCallback) {
    _whisperDevice = std::make_unique<WhillatsTranscriber>(_whisperModelFilename.c_str(), whisperCallback, languageCallback);
  return _whisperDevice.get();
}

WhillatsLlama* SpeechAudioDeviceFactory::CreateWhillatsLlama(WhillatsSetResponseCallback &llamaCallback) {
    _llamaDevice = std::make_unique<WhillatsLlama>(_llamaModelFilename.c_str(), _llavaMMProjFilename.c_str(), llamaCallback);
  return _llamaDevice.get();
}

void SpeechAudioDeviceFactory::SetWhisperModelFilename(absl::string_view whisper_model_filename) {
  _whisperModelFilename = whisper_model_filename;
}

void SpeechAudioDeviceFactory::SetLlamaModelFilename(absl::string_view llama_model_filename) {
  _llamaModelFilename = llama_model_filename;
}

void SpeechAudioDeviceFactory::SetLlavaMMProjFilename(absl::string_view llava_mmproj_filename) {
  _llavaMMProjFilename = llava_mmproj_filename;
}

void SpeechAudioDeviceFactory::SetWavFilename(absl::string_view wav_filename) {
  _wavFilename = wav_filename;
}

void SpeechAudioDeviceFactory::SetYuvFilename(absl::string_view yuv_filename) {
  _yuvFilename = yuv_filename;
}

void SpeechAudioDeviceFactory::SetTaskQueueFactory(TaskQueueFactory* task_queue_factory) {
  _taskQueueFactory = task_queue_factory;
}

AudioDeviceGeneric* SpeechAudioDeviceFactory::CreateSpeechAudioDevice() {
  WhisperAudioDevice* whisper_audio_device = nullptr;
  if(!whisper_audio_device) {

    // if(_whisperModelFilename.empty()) {
    //   SpeechAudioDeviceFactory::_whisperModelFilename = std::getenv("WHISPER_MODEL") ? \
    //     std::getenv("WHISPER_MODEL") : ""; // Must be ggml
    //   if(SpeechAudioDeviceFactory::_whisperModelFilename.empty())
    //     RTC_LOG(LS_WARNING)
    //       << "WHISPER_MODEL enviroment variable is empty! Did you mean it?";
    // }

    // if(_llamaModelFilename.empty()) {
    //   SpeechAudioDeviceFactory::_llamaModelFilename = std::getenv("LLAMA_MODEL") ? \
    //     std::getenv("LLAMA_MODEL") : ""; // Must be gguf
    //   if(SpeechAudioDeviceFactory::_llamaModelFilename.empty())
    //     RTC_LOG(LS_WARNING)
    //       << "LLAMA_MODEL enviroment variable is empty! Did you mean it?";
    // }

    // SpeechAudioDeviceFactory::_wavFilename = std::getenv("WEBRTC_SPEECH_INITIAL_PLAYOUT_WAV") ? \
    //   std::getenv("WEBRTC_SPEECH_INITIAL_PLAYOUT_WAV") : ""; // Must be .wav
    // if(!SpeechAudioDeviceFactory::_wavFilename.empty())
    //   RTC_LOG(LS_INFO)
    //     << "WEBRTC_SPEECH_INITIAL_PLAYOUT_WAV is '" << SpeechAudioDeviceFactory::_wavFilename << "'";

    whisper_audio_device = new WhisperAudioDevice(_taskQueueFactory);
    RTC_LOG(LS_INFO) << "Initialized WhisperAudioDevice instance.";
  }

  return static_cast<AudioDeviceGeneric*>(whisper_audio_device);
}

}  // namespace webrtc
