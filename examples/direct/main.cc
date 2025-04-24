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

#include <string>
#include <vector>
#include <unistd.h>

#include "direct.h"
#include "option.h"

int main(int argc, char* argv[]) {
  
  Options opts;
  std::string options;
  if(argc == 1) {
    opts.help = true;
  } else {
    std::vector<std::string> args(argv + 1, argv + argc);
    for (const auto &piece : args) options += (piece + " ");
    opts = parseOptions(options.c_str());
  } 

  if (opts.help) {
    auto usage = opts.help_string;
    // Print usage to stderr instead for command-line tools
    fprintf(stderr, "%s\n", usage.c_str()); 
    return 1;
  }

  DirectSetLoggingLevel(LoggingSeverity::LS_INFO); 
  DirectApplication::rtcInitialize();

  opts.mode = "callee";
  std::unique_ptr<DirectCallee> callee = std::make_unique<DirectCallee>(opts);
  if (!callee->Initialize()) {
    fprintf(stderr, "Failed to initialize callee\n");
    return 1;
  }
  if (!callee->StartListening()) {
    fprintf(stderr, "Failed to start listening\n");
    return 1;
  }
  callee->RunOnBackgroundThread(); 

  opts.mode = "caller";
  std::unique_ptr<DirectCaller> caller = std::make_unique<DirectCaller>(opts);
  if (!caller->Initialize()) {
    fprintf(stderr, "failed to initialize caller\n");
    return 1;
  }
  if (!caller->Connect()) {
    // RTC_LOG(LS_ERROR) << "failed to connect"; // Removed logging
    fprintf(stderr, "failed to connect\n");
    return 1;
  }
  caller->RunOnBackgroundThread();

  usleep(10000000);

  // Wait for user input or some condition to quit
  caller->Disconnect();
  usleep(500000);
  caller.Connect();

  // Signal both instances to quit before destruction
  callee->SignalQuit();
  caller->SignalQuit();

  callee.reset();
  caller.reset();

  // Allow some time for threads to process quit
  usleep(50000);

  DirectApplication::rtcCleanup();
  return 0;
}
