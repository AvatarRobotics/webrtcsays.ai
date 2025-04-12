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

#include <fstream>     // For file operations
#include <cerrno>      // For errno used with strtol

#include "rtc_base/logging.h"
#include "rtc_base/ref_counted_object.h"
#include "rtc_base/rtc_certificate_generator.h"
#include "rtc_base/system/rtc_export.h"
#include <json/json.h> // Use jsoncpp header

#include "option.h"

// Helper function to expand ${HOME} in paths
namespace {
std::string expandHomePath(const std::string& path) {
    if (path.rfind("${HOME}", 0) == 0) { // Check if path starts with ${HOME}
        const char* home_dir = std::getenv("HOME");
        if (home_dir) {
            std::string expanded_path = home_dir;
            // Append the rest of the path, handling the optional separator
            if (path.length() > 7 && (path[7] == '/' || path[7] == '\\')) {
                expanded_path += path.substr(7); // Skip ${HOME} and the separator
            } else if (path.length() > 7) { 
                 expanded_path += "/"; // Add separator if missing
                 expanded_path += path.substr(7); // Skip ${HOME}
            } else { 
                 // Path was just "${HOME}", nothing to append
            }
            return expanded_path;
        } else {
            RTC_LOG(LS_WARNING) << "HOME environment variable not set, cannot expand path: " << path;
            return path; // Return original path if HOME is not set
        }
    } 
    return path; // Return original path if it doesn't start with ${HOME}
}
} // namespace

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
  
  // Use strtol for exception-safe conversion
  const char* start_ptr = port_str.c_str();
  char* end_ptr = nullptr;
  errno = 0; // Reset errno
  long port_val = strtol(start_ptr, &end_ptr, 10);

  // Check for conversion errors and that the whole string was consumed
  if (errno != 0 || end_ptr == start_ptr || *end_ptr != '\0') {
    RTC_LOG(LS_ERROR) << "Invalid port string: " << port_str;
    return false;
  }

  // Check port range
  if (port_val < 0 || port_val > 65535) {
    RTC_LOG(LS_ERROR) << "Invalid port range: " << port_val;
    return false;
  }

  port = static_cast<int>(port_val); // Assign the valid port
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
  // Initialize defaults (ensure all relevant defaults are set here)
  opts.encryption = false;
  opts.whisper = false;
  opts.video = false; // Assuming default is false
  opts.mode = ""; 
  opts.webrtc_cert_path = "cert.pem";
  opts.webrtc_key_path = "key.pem";
  opts.webrtc_speech_initial_playout_wav = "play.wav";
  opts.help = false;
  // Restore original help string
  opts.help_string =
      "Usage:\n"
      "direct [options] address [options]\n\n"
      "Options:\n"
      "  --config <path>                  Load options from JSON config file.\n"
      "                                     Command-line options override config file.\n"
      "  --mode <caller|callee>             Set operation mode (default: caller)\n"
      "  --encryption, --no-encryption      Enable/disable encryption (default: disabled)\n"
      "  --whisper, --no-whisper            Enable/disable whisper (default: no-whisper)\n"
      "  --video, --no-video                Enable/disable video (default: disabled)\n" // Added video help
      "  --whisper_model=<path>             Path to whisper model\n"
      "  --llama_model=<path>               Path to llama model\n"
      "  --webrtc_cert_path=<path>          Path to WebRTC certificate (default: 'cert.pem')\n"
      "  --webrtc_key_path=<path>           Path to WebRTC key (default: 'key.pem')\n"
      "  --turns=<ip,username,password>     Secured turn server address, e.g. \n"
      "   'turns:global.relay.metered.ca:443?transport=tcp,<username>,<password>'\n"
      "  --vpn=<interface_name>           Specify VPN interface name\n" // Added VPN help
      "  --help                             Show this help message\n\n"
      "Examples (callee called first, encryption is recommended):\n"
      "  direct --config settings.json\n"
      "  direct --config settings.json --mode=callee --no-encryption\n"
      "  direct --mode=callee :3478 --no-encryption\n"
      "  direct --mode=callee 192.168.1.100:3478 --encryption --whisper"
      " --whisper_model=/path/to/model.bin --llama_model=/path/to/llama.gguf\n"
      "  direct --mode=caller 192.168.1.100:3478 --encryption\n"
      ;

  // --- First pass: check for --config --- 
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--config" && i + 1 < argc) {
      opts.config_path = argv[i + 1];
      break; // Found config, stop first pass
    } else if (arg.find("--config=") == 0) {
      opts.config_path = arg.substr(9);
      break; // Found config, stop first pass
    }
  }

  // --- Load from config file if specified ---
  // --- Re-enable JSON loading ---
  if (!opts.config_path.empty()) {
    // Use C-style file I/O to avoid potential std::ifstream issues
    FILE* fp = fopen(opts.config_path.c_str(), "rb");
    if (fp) {
      RTC_LOG(LS_VERBOSE) << "JSON C-IO: Config file opened successfully.";
      std::string contents;
      fseek(fp, 0, SEEK_END);
      contents.resize(ftell(fp));
      rewind(fp);
      fread(&contents[0], 1, contents.size(), fp);
      fclose(fp);
      RTC_LOG(LS_VERBOSE) << "JSON C-IO: Read " << contents.size() << " bytes.";

      Json::Value config_json;
      Json::CharReaderBuilder reader_builder;
      std::string errs;
      std::unique_ptr<Json::CharReader> reader(reader_builder.newCharReader());
      bool parsingSuccessful = reader->parse(
          contents.data(), contents.data() + contents.size(), 
          &config_json, &errs);
      RTC_LOG(LS_VERBOSE) << "JSON C-IO: Parsing result: " << (parsingSuccessful ? "Success" : "Failure"); // Log parse result

      if (parsingSuccessful) {
        RTC_LOG(LS_VERBOSE) << "JSON C-IO: Parsing succeeded. Checking members..."; // Log before member checks

        // Explicitly parse each option without macros
        // Strings
        if (config_json.isMember("mode") && config_json["mode"].isString()) {
             RTC_LOG(LS_INFO) << "JSON: Found mode: " << config_json["mode"].asString(); // Log value
             opts.mode = config_json["mode"].asString(); // Direct assignment (Uncommented)
        }
        if (config_json.isMember("whisper_model") && config_json["whisper_model"].isString()) {
             RTC_LOG(LS_INFO) << "JSON: Found whisper_model: " << config_json["whisper_model"].asString(); // Log value
             opts.whisper_model = expandHomePath(config_json["whisper_model"].asString());
        }
        if (config_json.isMember("llama_model") && config_json["llama_model"].isString()) {
             RTC_LOG(LS_INFO) << "JSON: Found llama_model: " << config_json["llama_model"].asString(); // Log value
             opts.llama_model = expandHomePath(config_json["llama_model"].asString());
        }
        if (config_json.isMember("webrtc_cert_path") && config_json["webrtc_cert_path"].isString()) {
             RTC_LOG(LS_INFO) << "JSON: Found webrtc_cert_path: " << config_json["webrtc_cert_path"].asString(); // Log value
             opts.webrtc_cert_path = expandHomePath(config_json["webrtc_cert_path"].asString());
        }
        if (config_json.isMember("webrtc_key_path") && config_json["webrtc_key_path"].isString()) {
             RTC_LOG(LS_INFO) << "JSON: Found webrtc_key_path: " << config_json["webrtc_key_path"].asString(); // Log value
             opts.webrtc_key_path = expandHomePath(config_json["webrtc_key_path"].asString());
        }
        if (config_json.isMember("webrtc_speech_initial_playout_wav") && config_json["webrtc_speech_initial_playout_wav"].isString()) {
             RTC_LOG(LS_INFO) << "JSON: Found initial playout_wav: " << config_json["webrtc_speech_initial_playout_wav"].asString(); // Log value
             opts.webrtc_speech_initial_playout_wav = expandHomePath(config_json["webrtc_speech_initial_playout_wav"].asString());
        }
        if (config_json.isMember("address") && config_json["address"].isString()) {
             RTC_LOG(LS_INFO) << "JSON: Found address: " << config_json["address"].asString(); // Log value
             opts.address = config_json["address"].asString();
        }
        if (config_json.isMember("turns") && config_json["turns"].isString()) {
             RTC_LOG(LS_INFO) << "JSON: Found turns params";
             opts.turns = config_json["turns"].asString();
        }
        if (config_json.isMember("vpn") && config_json["vpn"].isString()) {
             RTC_LOG(LS_INFO) << "JSON: Found vpn: " << config_json["vpn"].asString(); // Log value
             opts.vpn = config_json["vpn"].asString();
        }

        // Booleans
        if (config_json.isMember("encryption") && config_json["encryption"].isBool()) {
             RTC_LOG(LS_INFO) << "JSON: Found encryption: " << config_json["encryption"].asBool(); // Log value
             opts.encryption = config_json["encryption"].asBool();
        }
        if (config_json.isMember("whisper") && config_json["whisper"].isBool()) {
             RTC_LOG(LS_INFO) << "JSON: Found whisper: " << config_json["whisper"].asBool(); // Log value
             opts.whisper = config_json["whisper"].asBool();
        }
        if (config_json.isMember("llama") && config_json["llama"].isBool()) {
             RTC_LOG(LS_INFO) << "JSON: Found llama: " << config_json["llama"].asBool(); // Log value
             opts.llama = config_json["llama"].asBool();
        }
        if (config_json.isMember("video") && config_json["video"].isBool()) {
             RTC_LOG(LS_INFO) << "JSON: Found video: " << config_json["video"].asBool(); // Log value
             opts.video = config_json["video"].asBool();
        }

        RTC_LOG(LS_INFO) << "Loaded options from config file: " << opts.config_path;
      } else {
        RTC_LOG(LS_ERROR) << "Failed to parse config file " << opts.config_path << " from buffer: " << errs;
      }
    } else {
      RTC_LOG(LS_WARNING) << "Could not open config file with fopen: " << opts.config_path;
    }
  }
  // End of JSON loading

  // Helper function to check if string is an address
  auto isAddress = [](const std::string& str) {
    // Allow simple port numbers like ":3478"
    if (str.length() > 1 && str[0] == ':') {
        const char* start_ptr = str.c_str() + 1;
        char* end_ptr = nullptr;
        errno = 0; // Reset errno before calling strtol
        long port_val = strtol(start_ptr, &end_ptr, 10);
        // Check for conversion errors (errno != 0), 
        // ensure the whole string was consumed (*end_ptr == '\0'),
        // and check if the port is in the valid range.
        if (errno == 0 && end_ptr != start_ptr && *end_ptr == '\0' && port_val >= 0 && port_val <= 65535) {
            return true; // Looks like a valid port number
        } else {
            return false; // Conversion failed or invalid port range
        }
    }
    // Check for standard IP:PORT or hostname:PORT
    size_t colon_pos = str.find(':');
    return colon_pos != std::string::npos && colon_pos > 0 && colon_pos < str.length() - 1;
  };

  // --- Second pass: parse command-line arguments (overriding config) ---
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];

    // Skip the --config argument itself in the second pass
    if (arg == "--config" && i + 1 < argc) {
      i++; // Skip the path value as well
      continue;
    } else if (arg.find("--config=") == 0) {
      continue;
    }

    // Handle parameters with values
    if (arg.find("--mode=") == 0) {
      opts.mode = arg.substr(7);
    } else if (arg.find("--whisper_model=") == 0) {
      opts.whisper_model = arg.substr(16);  // Length of "--whisper_model="
      if (!opts.whisper_model.empty() && !opts.whisper)
        opts.whisper = true; // Enable whisper if model is provided
    } else if (arg.find("--llama_model=") == 0) {
      opts.llama_model = arg.substr(14);  // Length of "--llama_model="
    } else if (arg.find("--webrtc_cert_path=") == 0) {
      opts.webrtc_cert_path = arg.substr(19);
    } else if (arg.find("--webrtc_key_path=") == 0) {
      opts.webrtc_key_path = arg.substr(18);
    } else if (arg.find("--webrtc_speech_initial_playout_wav=") == 0) {
      opts.webrtc_speech_initial_playout_wav = arg.substr(36);
    } else if (arg.find("--turns=") == 0) {
      opts.turns = arg.substr(8);
      // Remove single quotes if present
      opts.turns.erase(remove(opts.turns.begin(), opts.turns.end(), '\''), opts.turns.end());
    } else if (arg.find("--vpn=") == 0) { 
        opts.vpn = arg.substr(6);
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
    } else if (arg == "--llama") {
      opts.llama = true;
    } else if (arg == "--no-llama") {
      opts.llama = false;
    } else if (arg == "--video") { 
      opts.video = true;
    } else if (arg == "--no-video") { 
      opts.video = false;
    }
    // Handle address in any position (must not be another known flag/option)
    else if (arg.rfind("--", 0) != 0 && isAddress(arg)) {
       opts.address = arg;
       // Only guess mode if not explicitly set or loaded from config
       if (opts.mode.empty()) {
           // Guess caller if it contains '.', otherwise callee (including simple port)
           opts.mode = (arg.find('.') != std::string::npos) ? "caller" : "callee";
       }
    } else if (arg.rfind("--", 0) == 0) {
        // Handle unknown flags if necessary, or log a warning
        RTC_LOG(LS_WARNING) << "Unknown option: " << arg;
    } else {
       // Treat as address if not starting with -- and isAddress passed? (Covered above)
       // Or potentially handle positional arguments if needed
    }
  }
  
  // Final check: If mode is still empty after parsing everything, default to caller
  if(opts.mode.empty()) {
      opts.mode = "caller";
  }

  // Restore environment variable handling
  // Load environment variables if paths not provided by command line or config
  // (Note: env vars are now lowest priority)
  if (opts.webrtc_cert_path.empty() || opts.webrtc_cert_path == "cert.pem") { // Check against default too
    if (const char* env_cert = std::getenv("WEBRTC_CERT_PATH")) {
      opts.webrtc_cert_path = env_cert;
    }
  }
  if (opts.webrtc_key_path.empty() || opts.webrtc_key_path == "key.pem") { // Check against default too
    if (const char* env_key = std::getenv("WEBRTC_KEY_PATH")) {
      opts.webrtc_key_path = env_key;
    }
  }
  if (opts.webrtc_speech_initial_playout_wav.empty() || opts.webrtc_speech_initial_playout_wav == "play.wav") { // Check against default too
    if (const char* env_wav =
            std::getenv("WEBRTC_SPEECH_INITIAL_PLAYOUT_WAV")) {
      opts.webrtc_speech_initial_playout_wav = env_wav;
    }
  }
  if (opts.whisper_model.empty()) {
    if (const char* env_whisper = std::getenv("WHISPER_MODEL")) {
      opts.whisper_model = env_whisper;
    }
  }
  // Set env var if whisper model is set (potentially from config or cmd line)
  if (!opts.whisper_model.empty()) {
    setenv("WHISPER_MODEL", opts.whisper_model.c_str(), 1); // Use 1 to overwrite
  }

  if (opts.llama_model.empty()) {
    if (const char* env_llama = std::getenv("LLAMA_MODEL")) {
      opts.llama_model = env_llama;
    }
  }
   // Set env var if llama model is set (potentially from config or cmd line)
  if (!opts.llama_model.empty()) {
    setenv("LLAMA_MODEL", opts.llama_model.c_str(), 1); // Use 1 to overwrite
  }
  if (opts.vpn.empty()) { 
    if (const char* env_vpn = std::getenv("VPN")) {
      opts.vpn = env_vpn;
    }
  }

  RTC_LOG(LS_INFO) << "Final effective mode: " << opts.mode; // Log final mode value
  return opts;
}

std::string getUsage(const Options opts) {
  std::stringstream usage;

  usage << "\\n--- Current Settings ---\\n"; // Header for clarity
  usage << "Mode: " << opts.mode << "\\n";
  usage << "Address: " << opts.address << "\\n";
  usage << "Encryption: " << (opts.encryption ? "enabled" : "disabled") << "\\n";
  usage << "Video: " << (opts.video ? "enabled" : "disabled") << "\\n";
  usage << "Whisper: " << (opts.whisper ? "enabled" : "disabled") << "\\n";
  if (opts.whisper) { // Only show model paths if whisper is enabled
      usage << "Whisper Model: " << (!opts.whisper_model.empty() ? opts.whisper_model : "(not set)") << "\\n";
      usage << "Llama Model: " << (!opts.llama_model.empty() ? opts.llama_model : "(not set)") << "\\n";
  }
  usage << "WebRTC Cert Path: " << opts.webrtc_cert_path << "\\n";
  usage << "WebRTC Key Path: " << opts.webrtc_key_path << "\\n";
  usage << "WebRTC Speech Initial Playout WAV: "
        << opts.webrtc_speech_initial_playout_wav << "\\n";
  usage << "TURN Server: " << (!opts.turns.empty() ? opts.turns : "(not set)") << "\\n";
  usage << "VPN Interface: " << (!opts.vpn.empty() ? opts.vpn : "(not set)") << "\\n";
  if (!opts.config_path.empty()) {
      usage << "Config File Used: " << opts.config_path << "\\n";
  }
   usage << "------------------------\\n"; // Footer

  return usage.str();
}
