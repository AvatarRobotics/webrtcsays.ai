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

#include <unistd.h>

#include <string>
#include <vector>
#include <climits>
#include <csignal>
#include <iostream>
#include <thread>
#include <chrono>

#include "direct.h"
#include "option.h"
#include "room.h"

static volatile bool g_shutdown = false;
static volatile int g_shutdown_count = 0;

// Signal handler for Ctrl+C
void signalHandler(int signal) {
    if (signal == SIGINT) {
        g_shutdown_count++;
        std::cout << "\nCtrl+C received (" << g_shutdown_count << "/3), shutting down...\n";
        g_shutdown = true;
        
        // Force immediate exit after 3 Ctrl+C presses to handle stuck scenarios
        if (g_shutdown_count >= 3) {
            std::cout << "Force exit after multiple Ctrl+C signals\n";
            std::cout.flush();
            _exit(1);  // Use _exit for immediate termination
        }
        
        // After 2nd Ctrl+C, be more aggressive
        if (g_shutdown_count >= 2) {
            std::cout << "Aggressive shutdown mode activated\n";
            std::cout.flush();
            // Set a 2-second timeout to force exit
            std::thread([]() {
                std::this_thread::sleep_for(std::chrono::seconds(2));
                std::cout << "Force exit due to timeout\n";
                std::cout.flush();
                _exit(1);
            }).detach();
        }
    }
}

int main(int argc, char* argv[]) {

  Options opts;
  std::string options;
  if (argc == 1) {
    opts.help = true;
  } else {
    std::vector<std::string> args(argv + 1, argv + argc);
    for (const auto& piece : args)
      options += (piece + " ");
    opts = parseOptions(options.c_str());
  }

  if (opts.help) {
    auto usage = opts.help_string;
    // Print usage to stderr instead for command-line tools
    fprintf(stderr, "%s\n", usage.c_str());
    return 1;
  }

  // Install signal handler for Ctrl+C
  signal(SIGINT, signalHandler);

  //DirectSetLoggingLevel(AS_INFO);
  DirectApplication::rtcInitialize();

  std::unique_ptr<DirectCallee> callee;
  std::unique_ptr<DirectCaller> caller;
  std::unique_ptr<RoomCaller> room;
  if (opts.mode == "callee" or opts.mode == "both") {
    int session_count = 0;
    while (!g_shutdown) {
      session_count++;
      fprintf(stderr, "Starting callee session #%d\n", session_count);
      
      callee = std::make_unique<DirectCallee>(opts);
      if (!callee->Initialize()) {
        fprintf(stderr, "failed to initialize callee\n");
        return 1;
      }
      if (!callee->StartListening()) {
        fprintf(stderr, "Failed to start listening\n");
        return 1;
      }
      callee->RunOnBackgroundThread();

      // Prepare new session and wait for CANCEL or Ctrl+C
      callee->ResetConnectionClosedEvent();
      fprintf(stderr, "Callee ready for incoming connections...\n");
      
      while (!g_shutdown) {
        // Check shutdown more frequently with shorter timeout
        if (callee->WaitUntilConnectionClosed(200)) {
          fprintf(stderr, "Callee session #%d ended (connection closed/failed), restarting listener\n", session_count);
          break;
        }
        
        // Check shutdown flag multiple times per second
        if (g_shutdown) {
          fprintf(stderr, "Shutdown requested during callee session #%d\n", session_count);
          break;
        }
      }
      // Signal quit immediately if shutdown is requested
      if (g_shutdown && callee) {
        fprintf(stderr, "Shutdown requested - signaling callee quit\n");
        callee->SignalQuit();
      }
      
      callee.reset();
      
      if (!g_shutdown) {
        fprintf(stderr, "Preparing to restart callee in 2 seconds...\n");
        // Check for shutdown during sleep as well
        for (int i = 0; i < 20 && !g_shutdown; i++) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
      }
    }
  }

  if(opts.mode == "caller" or opts.mode == "both") {
    opts.mode = "caller";
    caller = std::make_unique<DirectCaller>(opts);
    if (!caller->Initialize()) {
      fprintf(stderr, "failed to initialize caller\n");
      return 1;
    }
    if (!caller->Connect()) {
      fprintf(stderr, "failed to connect\n");
      return 1;
    }
    caller->RunOnBackgroundThread();
  }

  if(opts.mode == "room") {
    room = std::make_unique<RoomCaller>(opts);
    if (!room->Initialize()) {
      fprintf(stderr, "failed to initialize room\n");
      return 1;
    }
    if (!room->Connect()) {
      fprintf(stderr, "failed to connect\n");
      return 1;
    }    
    room->RunOnBackgroundThread();
  }

  while (!g_shutdown) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    // Additional shutdown check for stuck scenarios
    if (g_shutdown_count >= 2) {
      fprintf(stderr, "Aggressive shutdown - breaking main loop\n");
      break;
    }
  }

  // Cleanup phase
  fprintf(stderr, "Starting cleanup...\n");
  
  if(opts.mode == "caller" or opts.mode == "both") {
    if (caller) {
      caller->Disconnect();
      // Shorter timeout for cleanup, don't wait too long
      int cleanup_timeout = g_shutdown_count >= 2 ? 1000 : 5000;
      if (caller->WaitUntilConnectionClosed(cleanup_timeout)) {
        fprintf(stderr, "Caller connection closed\n");
      } else {
        fprintf(stderr, "Caller connection not closed after timeout\n");
      }
    }
  }

  if(opts.mode == "callee" or opts.mode == "both") {
    if (callee) {
      fprintf(stderr, "Signaling callee quit...\n");
      callee->SignalQuit();
      callee.reset();
      fprintf(stderr, "Callee cleanup complete\n");
    }
  }

  if(opts.mode == "caller" or opts.mode == "both") {
    if (caller) {
      fprintf(stderr, "Signaling caller quit...\n");
      caller->SignalQuit();
      caller.reset();
      fprintf(stderr, "Caller cleanup complete\n");
    }
  }

  // Allow some time for threads to process quit, but not too long in aggressive mode
  int sleep_time = g_shutdown_count >= 2 ? 10000 : 50000;
  fprintf(stderr, "Waiting %d microseconds for thread cleanup...\n", sleep_time);
  usleep(sleep_time);

  DirectApplication::rtcCleanup();
  return 0;
}
