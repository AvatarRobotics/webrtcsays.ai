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

#pragma once

#include <memory>
#include <string>
#include <map>
#include <vector>
#include <thread>
#include <atomic>
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <cstdlib> // for std::getenv
#include <cstdio>  // for FILE, fopen, fclose
#include <fstream>
#include <sstream>
#include <future>
#include <algorithm>
#include <cstdlib> // for getenv

#include <sys/socket.h>
#include <netinet/tcp.h>

#include "rtc_base/ssl_certificate.h"

#ifndef DIRECT_EXPORT_H
#define DIRECT_EXPORT_H

#if defined(_MSC_VER)
    #define WHILLATS_EXPORT __declspec(dllexport)
    #define WHILLATS_IMPORT __declspec(dllimport)
#elif defined(__GNUC__)
    #define DIRECT_EXPORT __attribute__((visibility("default")))
    #define DIRECT_IMPORT __attribute__((visibility("default")))
#else
    #define DIRECT_EXPORT
    #define DIRECT_IMPORT
#endif

#ifdef DIRECT_BUILDING_DLL
    #define DIRECT_API DIRECT_EXPORT
#else
    #define DIRECT_API DIRECT_IMPORT
#endif

enum LoggingSeverity {
  LS_VERBOSE,
  LS_INFO,
  LS_WARNING,
  LS_ERROR,
  LS_NONE,
};

#define AS_VERBOSE LS_VERBOSE
#define AS_INFO LS_INFO
#define AS_WARNING LS_WARNING
#define AS_ERROR LS_ERROR
#define AS_NONE LS_NONE
#define APP_LOG(x) RTC_LOG(x)

#endif // DIRECT_EXPORT_H 

// Command line options
struct Options {
    std::string mode = "caller"; // default to caller if not specified
    bool encryption = true;
    bool whisper = true;
    bool llama = true;
    bool video = false;
    bool help = false;
    std::string help_string;
    std::string config_path = ""; // Path to JSON config file
    std::string whisper_model;
    std::string llama_model;
    std::string llava_mmproj;
    std::string webrtc_cert_path = "cert.pem";
    std::string webrtc_key_path = "key.pem";
    std::string webrtc_speech_initial_playout_wav = "play.wav";
    std::string llama_llava_yuv = "test.yuv";
    int llama_llava_yuv_width = 300;
    int llama_llava_yuv_height = 300;
    std::string address = "";
    std::string turns = "";
    std::string vpn = "";
};

// Function to parse command line string to above options
DIRECT_API Options parseOptions(const char* argString);
Options parseOptions(const std::vector<std::string>& args);

bool ParseIpAndPort(const std::string& ip_port, std::string& ip, int& port);

// Function to get command line options to a string, to print or speak
DIRECT_API std::string getUsage(const Options opts);

DIRECT_API void DirectSetLoggingLevel(LoggingSeverity level);

DIRECT_API rtc::scoped_refptr<rtc::RTCCertificate> DirectLoadCertificateFromEnv(Options opts);

DIRECT_API uint32_t DirectCreateRandomId();

DIRECT_API std::string DirectCreateRandomUuid();

DIRECT_API void DirectThreadSetName(rtc::Thread* thread, const char* name);

DIRECT_API webrtc::IceCandidateInterface* DirectCreateIceCandidate(const char* sdp_mid,
    int sdp_mline_index,
    const char* sdp,
    webrtc::SdpParseError* error);

DIRECT_API std::unique_ptr<webrtc::SessionDescriptionInterface> DirectCreateSessionDescription(
    webrtc::SdpType type,
    const char* sdp,
    webrtc::SdpParseError* error_out);
