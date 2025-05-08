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

#include "direct.h"
#include "option.h"
#include "room.h"

static int g_shutdown = 0;

// Signal handler for Ctrl+C
void signalHandler(int signal) {
    if (signal == SIGINT) {
        std::cout << "\nCtrl+C received, shutting down...\n";
        g_shutdown = 1;
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

  DirectSetLoggingLevel(LoggingSeverity::LS_VERBOSE);
  DirectApplication::rtcInitialize();

  std::unique_ptr<DirectCallee> callee;
  std::unique_ptr<DirectCaller> caller;
  std::unique_ptr<RoomCaller> room;
  if(opts.mode == "callee" or opts.mode == "both") {
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
    usleep(100000);
  }

  if(opts.mode == "caller" or opts.mode == "both") {
    caller->Disconnect();
    if (caller->WaitUntilConnectionClosed(5000)) {
      fprintf(stderr, "Connection closed");
    } else {
      fprintf(stderr, "Connection not closed after 5 seconds");
    }
  }

  if(opts.mode == "callee" or opts.mode == "both") {
    callee->SignalQuit();
    callee.reset();
  }

  if(opts.mode == "caller" or opts.mode == "both") {
    caller->SignalQuit();
    caller.reset();
  }

  // Allow some time for threads to process quit
  usleep(50000);

  DirectApplication::rtcCleanup();
  return 0;
}
