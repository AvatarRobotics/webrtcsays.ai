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

#endif // DIRECT_EXPORT_H 


// Function to parse IP address and port from a string in the format "IP:PORT"
bool ParseIpAndPort(const std::string& ip_port, std::string& ip, int& port);

// String split
std::vector<std::string> stringSplit(std::string input, std::string delimiter);

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
    std::string webrtc_cert_path = "cert.pem";
    std::string webrtc_key_path = "key.pem";
    std::string webrtc_speech_initial_playout_wav = "play.wav";
    std::string address = "";
    std::string turns = "";
    std::string vpn = "";
};

// Function to parse command line string to above options
DIRECT_API Options parseOptions(const std::vector<std::string>& args);

// Function to get command line options to a string, to print or speak
DIRECT_API std::string getUsage(const Options opts);