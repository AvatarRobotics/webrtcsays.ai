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

#include "direct.h"
#include <string>
#include <vector>
#include <unistd.h>
#include <cstdio>

int main(int argc, char* argv[]) {
  
  std::vector<std::string> args(argv + 1, argv + argc);
  std::string s;
  for (const auto &piece : args) {
    s += (piece + " ");
  }
  
  Options opts = parseOptions(s.c_str());

  if (opts.help) {
    auto usage = opts.help_string;
    // Print usage to stderr instead for command-line tools
    fprintf(stderr, "%s\n", usage.c_str()); 
    return 1;
  }

  DirectApplication::rtcInitializeSSL();

  opts.mode = "callee";
  DirectCallee callee(opts);
  if (!callee.Initialize()) {
    fprintf(stderr, "Failed to initialize callee\n");
    return 1;
  }
  if (!callee.StartListening()) {
    fprintf(stderr, "Failed to start listening\n");
    return 1;
  }
  callee.RunOnBackgroundThread(); 

  opts.mode = "caller";
  DirectCaller caller(opts);
  if (!caller.Initialize()) {
    fprintf(stderr, "failed to initialize caller\n");
    return 1;
  }
  if (!caller.Connect()) {
    // RTC_LOG(LS_ERROR) << "failed to connect"; // Removed logging
    fprintf(stderr, "failed to connect\n");
    return 1;
  }
  caller.RunOnBackgroundThread();

  usleep(500000);

  for (int i = 0; i < 10; i++) {

    // Now disconnect the first connection before reconnecting
    caller.Disconnect();
    // Allow a brief moment for disconnect messages to potentially be sent/processed
    usleep(500000); 

    // Attempt the second connection (reconnect)
    fprintf(stderr, "Attempting reconnect...\n");
    if (!caller.Connect()) {
        fprintf(stderr, "Reconnect failed\n");
        // Decide how to handle reconnect failure (e.g., retry, quit)
    } else {
        fprintf(stderr, "Reconnect initiated.\n");
        // Caller background thread is already running, should handle the new connection state
    }
    usleep(5000000);
  }

  // Wait for user input or some condition to quit
  fprintf(stderr, "Press Enter to quit...\n");
  getchar();

  // Signal both instances to quit before destruction
  callee.SignalQuit();
  caller.SignalQuit();

  // Allow some time for threads to process quit
  usleep(50000);

  DirectApplication::rtcCleanupSSL();
  return 0;
}
