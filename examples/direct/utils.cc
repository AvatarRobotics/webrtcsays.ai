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

#include "utils.h"
#include "rtc_base/logging.h"
#include "rtc_base/ref_counted_object.h"
#include "rtc_base/rtc_certificate_generator.h"
#include "rtc_base/system/rtc_export.h"

#if defined(WEBRTC_LINUX)
extern "C" {
void __libc_csu_init() {}
void __libc_csu_fini() {}
}
#endif

// Function to parse IP address and port from a string in the format "IP:PORT"
bool ParseIpAndPort(const std::string& ip_port, std::string& ip, int& port) {
  size_t colon_pos = ip_port.find(':');
  if (colon_pos == std::string::npos) {
    RTC_LOG(LS_ERROR) << "Invalid IP:PORT format: " << ip_port;
    return false;
  }

  ip = ip_port.substr(0, colon_pos);
  std::string port_str = ip_port.substr(colon_pos + 1);
  port = std::stoi(port_str);

  if (port < 0 || port > 65535) {
    RTC_LOG(LS_ERROR) << "Invalid port: " << port;
    return false;
  }

  return true;
}

std::vector<std::string> stringSplit(std::string input, std::string delimiter)
{
    std::vector<std::string> tokens;
    size_t pos = 0;
    std::string token;

    while((pos = input.find(delimiter)) != std::string::npos){
        token = input.substr(0, pos);
        tokens.push_back(token);
        input.erase(0, pos + 1);
    }

    tokens.push_back(input);
    return tokens;
}

// Function to parse command line string to above options
Options parseOptions(int argc, char* argv[]) {
  Options opts;
  // Initialize defaults
  opts.encryption = false;
  opts.whisper = false;
  opts.mode = "";
  opts.webrtc_cert_path = "cert.pem";
  opts.webrtc_key_path = "key.pem";
  opts.webrtc_speech_initial_playout_wav = "play.wav";
  opts.help = false;
  opts.help_string =
      "Usage:\n"
      "direct [options] address [options]\n\n"
      "Options:\n"
      "  --mode <caller|callee>             Set operation mode (default: caller)\n"
      "  --encryption, --no-encryption      Enable/disable encryption (default: disabled)\n"
      "  --whisper, --no-whisper            Enable/disable whisper (default: no-whisper)\n"
      "  --whisper_model=<path>             Path to whisper model\n"
      "  --llama_model=<path>               Path to llama model\n"
      "  --webrtc_cert_path=<path>          Path to WebRTC certificate (default: 'cert.pem')\n"
      "  --webrtc_key_path=<path>           Path to WebRTC key (default: 'key.pem')\n"
      "  --turns=<ip,username,password>     Secured turn server address, e.g. \n"
      "   'turns:global.relay.metered.ca:443?transport=tcp,<username>,<password>'\n"
      "  --help                             Show this help message\n\n"
      "\nExamples (callee called first, encryption is recommended):\n"
      "  direct --mode=callee :3478 --no-encryption\n"
      "  direct --mode=callee 192.168.1.100:3478 --encryption --whisper"
      "--whisper_model=/path/to/model.bin --llama_model=/path/to/llama.gguf\n\n"
      "  direct --mode=caller 192.168.1.100:3478 --encryption\n"
      ;

  // Helper function to check if string is an address
  auto isAddress = [](const std::string& str) {
    return str.find(':') != std::string::npos &&
           (str.find('.') != std::string::npos || str[0] == ':');
  };

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];

    // Handle parameters with values
    if (arg.find("--mode=") == 0) {
      opts.mode = arg.substr(7);
    } else if (arg == "--encryption") {
      opts.encryption = true;
    } else if (arg == "--whisper") {
      opts.whisper = true;
    } else if (arg.find("--whisper_model=") == 0) {
      opts.whisper_model = arg.substr(16);  // Length of "-whisper_model="
      RTC_LOG(LS_INFO) << "Whisper model path: " << opts.whisper_model;
      if (!opts.whisper)
        opts.whisper = true;
    } else if (arg.find("--llama_model=") == 0) {
      opts.llama_model = arg.substr(14);  // Length of "-llama_model="
      RTC_LOG(LS_INFO) << "LLAMA model path: " << opts.llama_model;
    } else if (arg.find("--webrtc_cert_path=") == 0) {
      opts.webrtc_cert_path = arg.substr(19);
    } else if (arg.find("--webrtc_key_path=") == 0) {
      opts.webrtc_key_path = arg.substr(18);
    } else if (arg.find("--webrtc_speech_initial_playout_wav=") == 0) {
      opts.webrtc_speech_initial_playout_wav = arg.substr(36);
    } else if (arg.find("--turns=") == 0) {
      opts.turns = arg.substr(8);
      // Remove single quotes
      opts.turns.erase(remove(opts.turns.begin(), opts.turns.end(), '\'' ), opts.turns.end());
    }
    // Handle flags
    else if (arg == "--help") {
      opts.help = true;
    } else if (arg == "--encryption") {
      opts.encryption = true;
    } else if (arg == "--no-encryption") {
      opts.encryption = false;
    } else if (arg == "--whisper") {
      opts.whisper = true;
    } else if (arg == "--no-whisper") {
      opts.whisper = false;
    }
    // Handle address in any position
    else if (isAddress(arg)) {
      opts.address = arg;
      // Only guess mode if not explicitly set
      if (opts.mode.empty()) {
        opts.mode = (arg.find('.') == std::string::npos) ? "callee" : "caller";
      }
    }
  }

  // Load environment variables if paths not provided
  if (opts.webrtc_cert_path.empty()) {
    if (const char* env_cert = std::getenv("WEBRTC_CERT_PATH")) {
      opts.webrtc_cert_path = env_cert;
    }
  }
  if (opts.webrtc_key_path.empty()) {
    if (const char* env_key = std::getenv("WEBRTC_KEY_PATH")) {
      opts.webrtc_key_path = env_key;
    }
  }
  if (opts.webrtc_speech_initial_playout_wav.empty()) {
    if (const char* env_wav =
            std::getenv("WEBRTC_SPEECH_INITIAL_PLAYOUT_WAV")) {
      opts.webrtc_speech_initial_playout_wav = env_wav;
    }
  }
  if (opts.whisper_model.empty()) {
    if (const char* env_whisper = std::getenv("WHISPER_MODEL")) {
      opts.whisper_model = env_whisper;
    }
  } else {
    setenv("WHISPER_MODEL", opts.whisper_model.c_str(), true);
  }

  if (opts.llama_model.empty()) {
    if (const char* env_llama = std::getenv("LLAMA_MODEL")) {
      opts.llama_model = env_llama;
    }
  } else {
    setenv("LLAMA_MODEL", opts.llama_model.c_str(), true);
  }

  return opts;
}

std::string getUsage(const Options opts) {
  std::stringstream usage;

  usage << "\nMode: " << opts.mode << "\n";
  usage << "Encryption: " << (opts.encryption ? "enabled" : "disabled") << "\n";
  usage << "Whisper: " << (opts.whisper ? "enabled" : "disabled") << "\n";
  usage << "Whisper Model: " << opts.whisper_model << "\n";
  usage << "Llama Model: " << opts.llama_model << "\n";
  usage << "WebRTC Cert Path: " << opts.webrtc_cert_path << "\n";
  usage << "WebRTC Key Path: " << opts.webrtc_key_path << "\n";
  usage << "WebRTC Speech Initial Playout WAV: "
        << opts.webrtc_speech_initial_playout_wav << "\n";
  usage << "IP Address: " << opts.address << "\n";

  return usage.str();
}
